/*
 * r8slog.sys - r8s-woa Windows -> firmware log handoff (KMDF).
 *
 * Binds to ACPI\RWOA0002 (DSDT \_SB.WLOG), whose _CRS is a reserved RAM
 * region that survives a warm reset. Anything written to \\.\R8sLog
 * (e.g. "pnputil /enum-devices > \\.\R8sLog") is appended there. After
 * Windows reboots, the firmware's R8sDiag.efi saves it as \r8s-winlog.txt.
 *
 * Some bits of DRAM flip during the reset, so the region holds WINLOG_COPIES
 * identical slices (64-byte header + text) and the firmware takes a bitwise
 * majority vote. Shared layout: see R8sDiag.c.
 *
 * The driver also writes a line every 5 s ("alive t=...") and, if Windows
 * bugchecks, the stop code, so a crash or hang shows in the log. Every 5 s
 * it also copies new text from X:\Windows\r8s-log.txt, which the startup
 * script writes, so the script never has to open the device.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <ntddk.h>
#include <wdf.h>

#define WINLOG_MAGIC   0x31474F4C57533852ULL      /* "R8SWLOG1" */
#define WINLOG_COPIES  5

typedef struct _WINLOG_HEADER {
    ULONG64 Magic;
    ULONG   Length;       /* bytes of text after the header */
    ULONG   Capacity;
    ULONG   Writes;
    ULONG   Reserved[11];
} WINLOG_HEADER;

C_ASSERT(sizeof(WINLOG_HEADER) == 64);

typedef struct _DEVICE_CONTEXT {
    volatile UCHAR  *Base;
    SIZE_T          Size;
    SIZE_T          Slice;        /* bytes per copy */
    ULONG           Length;       /* authoritative copy of the header fields */
    ULONG           Capacity;
    ULONG           Writes;
    WDFSPINLOCK     Lock;
    WDFTIMER        Timer;
    WDFWORKITEM     WorkItem;
    LONGLONG        Mirrored;     /* bytes of \SystemRoot\r8s-log.txt already copied */
    BOOLEAN         BugCheckRegistered;
    KBUGCHECK_CALLBACK_RECORD BugCheckRecord;
} DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetContext)

DRIVER_INITIALIZE                   DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD           R8sLogDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE     R8sLogPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE     R8sLogReleaseHardware;
EVT_WDF_IO_QUEUE_IO_WRITE           R8sLogWrite;
EVT_WDF_IO_QUEUE_IO_READ            R8sLogRead;
EVT_WDF_TIMER                       R8sLogTimer;
EVT_WDF_WORKITEM                    R8sLogMirrorFile;
KBUGCHECK_CALLBACK_ROUTINE          R8sLogBugCheck;

static DEVICE_CONTEXT *gCtx;           /* for the bugcheck callback */
static ULONG_PTR      *gBugCheckData;  /* ntoskrnl's KiBugCheckData, if exported */

/* --- raw log access (no locking; callers serialise) ------------------- */

static void
StoreHeader(DEVICE_CONTEXT *Ctx)
{
    WINLOG_HEADER Hdr;
    ULONG         c, i;

    RtlZeroMemory(&Hdr, sizeof(Hdr));
    Hdr.Magic    = WINLOG_MAGIC;
    Hdr.Length   = Ctx->Length;
    Hdr.Capacity = Ctx->Capacity;
    Hdr.Writes   = Ctx->Writes;

    for (c = 0; c < WINLOG_COPIES; c++) {
        for (i = 0; i < sizeof(Hdr); i++) {
            Ctx->Base[c * Ctx->Slice + i] = ((const UCHAR *)&Hdr)[i];
        }
    }
}

static void
AppendRaw(DEVICE_CONTEXT *Ctx, const UCHAR *Data, SIZE_T Len)
{
    SIZE_T Room, i;
    ULONG  c;

    if ((Ctx->Base == NULL) || (Ctx->Length >= Ctx->Capacity)) {
        return;
    }

    Room = Ctx->Capacity - Ctx->Length;
    if (Len > Room) {
        Len = Room;
    }

    /* Device (non-cached) memory: plain byte stores into every copy. */
    for (c = 0; c < WINLOG_COPIES; c++) {
        volatile UCHAR *Dst = Ctx->Base + c * Ctx->Slice + sizeof(WINLOG_HEADER) + Ctx->Length;
        for (i = 0; i < Len; i++) {
            Dst[i] = Data[i];
        }
    }

    Ctx->Length += (ULONG)Len;
    Ctx->Writes++;
    StoreHeader(Ctx);
}

static SIZE_T
StrLen(const char *Text)
{
    SIZE_T Len = 0;
    while (Text[Len] != '\0') {
        Len++;
    }

    return Len;
}

/* Tiny formatter: decimal and hex numbers into a line, no CRT needed (any IRQL). */
static char *
PutStr(char *P, char *End, const char *S)
{
    while ((*S != '\0') && (P < End)) {
        *P++ = *S++;
    }

    return P;
}

static char *
PutDec(char *P, char *End, ULONG64 V)
{
    char  Tmp[24];
    int   n = 0;

    do {
        Tmp[n++] = (char)('0' + (V % 10));
        V /= 10;
    } while ((V != 0) && (n < (int)sizeof(Tmp)));

    while ((n > 0) && (P < End)) {
        *P++ = Tmp[--n];
    }

    return P;
}

static char *
PutHex(char *P, char *End, ULONG64 V, int Digits)
{
    static const char Hex[] = "0123456789abcdef";
    int i;

    P = PutStr(P, End, "0x");
    for (i = Digits - 1; (i >= 0) && (P < End); i--) {
        *P++ = Hex[(V >> (i * 4)) & 0xF];
    }

    return P;
}

static void
AppendLine(DEVICE_CONTEXT *Ctx, const char *Text, BOOLEAN Locked)
{
    if (Locked) {
        WdfSpinLockAcquire(Ctx->Lock);
    }

    AppendRaw(Ctx, (const UCHAR *)Text, StrLen(Text));

    if (Locked) {
        WdfSpinLockRelease(Ctx->Lock);
    }
}

/* --- driver ------------------------------------------------------------ */

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    UNICODE_STRING    Name;

    RtlInitUnicodeString(&Name, L"KiBugCheckData");
    gBugCheckData = (ULONG_PTR *)MmGetSystemRoutineAddress(&Name);

    WDF_DRIVER_CONFIG_INIT(&Config, R8sLogDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}

NTSTATUS
R8sLogDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS                     Status;
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES        Attr;
    WDF_IO_QUEUE_CONFIG          QueueConfig;
    WDF_TIMER_CONFIG             TimerConfig;
    WDFDEVICE                    Device;
    DEVICE_CONTEXT               *Ctx;
    DECLARE_CONST_UNICODE_STRING(Name, L"\\Device\\R8sLog");
    DECLARE_CONST_UNICODE_STRING(Link, L"\\DosDevices\\R8sLog");

    UNREFERENCED_PARAMETER(Driver);

    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);
    Status = WdfDeviceInitAssignName(DeviceInit, &Name);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = R8sLogPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = R8sLogReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, DEVICE_CONTEXT);
    Status = WdfDeviceCreate(&DeviceInit, &Attr, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Ctx = GetContext(Device);
    RtlZeroMemory(Ctx, sizeof(*Ctx));

    WDF_OBJECT_ATTRIBUTES_INIT(&Attr);
    Attr.ParentObject = Device;
    Status = WdfSpinLockCreate(&Attr, &Ctx->Lock);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    WDF_TIMER_CONFIG_INIT_PERIODIC(&TimerConfig, R8sLogTimer, 5000);
    TimerConfig.AutomaticSerialization = FALSE;     /* own spinlock */
    WDF_OBJECT_ATTRIBUTES_INIT(&Attr);
    Attr.ParentObject = Device;
    Status = WdfTimerCreate(&TimerConfig, &Attr, &Ctx->Timer);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    {
        WDF_WORKITEM_CONFIG WorkConfig;

        WDF_WORKITEM_CONFIG_INIT(&WorkConfig, R8sLogMirrorFile);
        WorkConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT(&Attr);
        Attr.ParentObject = Device;
        Status = WdfWorkItemCreate(&WorkConfig, &Attr, &Ctx->WorkItem);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    Status = WdfDeviceCreateSymbolicLink(Device, &Link);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchSequential);
    QueueConfig.EvtIoWrite = R8sLogWrite;
    QueueConfig.EvtIoRead  = R8sLogRead;
    return WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}

NTSTATUS
R8sLogPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw, _In_ WDFCMRESLIST Translated)
{
    DEVICE_CONTEXT                  *Ctx = GetContext(Device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc;
    ULONG                           i, Count;
    char                            Line[128], *P, *End = Line + sizeof(Line) - 1;

    UNREFERENCED_PARAMETER(Raw);

    Count = WdfCmResourceListGetCount(Translated);
    for (i = 0; i < Count; i++) {
        Desc = WdfCmResourceListGetDescriptor(Translated, i);
        if ((Desc != NULL) && (Desc->Type == CmResourceTypeMemory) && (Desc->u.Memory.Length > WINLOG_COPIES * PAGE_SIZE)) {
            Ctx->Size = Desc->u.Memory.Length;
            Ctx->Base = (volatile UCHAR *)MmMapIoSpaceEx(Desc->u.Memory.Start, Ctx->Size, PAGE_READWRITE | PAGE_NOCACHE);
            break;
        }
    }

    if (Ctx->Base == NULL) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* Fresh log every boot: the firmware has saved (and cleared) the last one. */
    Ctx->Slice    = (Ctx->Size / WINLOG_COPIES) & ~(SIZE_T)(PAGE_SIZE - 1);
    Ctx->Capacity = (ULONG)(Ctx->Slice - sizeof(WINLOG_HEADER));
    Ctx->Length   = 0;
    Ctx->Writes   = 0;
    StoreHeader(Ctx);
    gCtx = Ctx;

    P = PutStr(Line, End, "\r\n[r8slog] driver started, ");
    P = PutDec(P, End, WINLOG_COPIES);
    P = PutStr(P, End, " copies x ");
    P = PutDec(P, End, Ctx->Capacity);
    P = PutStr(P, End, " bytes, bugcheck data ");
    P = PutStr(P, End, (gBugCheckData != NULL) ? "found" : "not exported");
    P = PutStr(P, End, "\r\n");
    *P = '\0';
    AppendLine(Ctx, Line, TRUE);

    KeInitializeCallbackRecord(&Ctx->BugCheckRecord);
    Ctx->BugCheckRegistered = KeRegisterBugCheckCallback(&Ctx->BugCheckRecord, R8sLogBugCheck, NULL, 0, (PUCHAR)"r8slog");

    WdfTimerStart(Ctx->Timer, WDF_REL_TIMEOUT_IN_MS(5000));
    return STATUS_SUCCESS;
}

NTSTATUS
R8sLogReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    DEVICE_CONTEXT *Ctx = GetContext(Device);

    UNREFERENCED_PARAMETER(Translated);

    WdfTimerStop(Ctx->Timer, TRUE);
    WdfWorkItemFlush(Ctx->WorkItem);
    if (Ctx->BugCheckRegistered) {
        KeDeregisterBugCheckCallback(&Ctx->BugCheckRecord);
        Ctx->BugCheckRegistered = FALSE;
    }

    gCtx = NULL;
    if (Ctx->Base != NULL) {
        MmUnmapIoSpace((PVOID)Ctx->Base, Ctx->Size);
        Ctx->Base = NULL;
    }

    return STATUS_SUCCESS;
}

VOID
R8sLogTimer(_In_ WDFTIMER Timer)
{
    DEVICE_CONTEXT *Ctx = GetContext(WdfTimerGetParentObject(Timer));
    char           Line[64], *P, *End = Line + sizeof(Line) - 1;

    P = PutStr(Line, End, "[r8slog] alive t=");
    P = PutDec(P, End, KeQueryInterruptTime() / 10000000ULL);
    P = PutStr(P, End, " s\r\n");
    *P = '\0';
    AppendLine(Ctx, Line, TRUE);

    WdfWorkItemEnqueue(Ctx->WorkItem);     /* copy new text from the file (PASSIVE_LEVEL) */
}

/*
 * The startup script writes to a normal file, X:\Windows\r8s-log.txt
 * (\SystemRoot\r8s-log.txt): cmd.exe redirection to a file always works,
 * and anything new in it is copied into the log RAM here every 5 s.
 */
VOID
R8sLogMirrorFile(_In_ WDFWORKITEM WorkItem)
{
    DEVICE_CONTEXT    *Ctx = GetContext(WdfWorkItemGetParentObject(WorkItem));
    UNICODE_STRING    Path;
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK   Iosb;
    HANDLE            File;
    LARGE_INTEGER     Offset;
    NTSTATUS          Status;
    static UCHAR      Buffer[16 * 1024];    /* work items run one at a time */

    RtlInitUnicodeString(&Path, L"\\SystemRoot\\r8s-log.txt");
    InitializeObjectAttributes(&Oa, &Path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile(&File, GENERIC_READ | SYNCHRONIZE, &Oa, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                          FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(Status)) {
        return;
    }

    for (;;) {
        Offset.QuadPart = Ctx->Mirrored;
        Status = ZwReadFile(File, NULL, NULL, NULL, &Iosb, Buffer, sizeof(Buffer), &Offset, NULL);
        if (!NT_SUCCESS(Status) || (Iosb.Information == 0)) {
            break;
        }

        WdfSpinLockAcquire(Ctx->Lock);
        AppendRaw(Ctx, Buffer, Iosb.Information);
        WdfSpinLockRelease(Ctx->Lock);
        Ctx->Mirrored += (LONGLONG)Iosb.Information;
    }

    ZwClose(File);
}

VOID
R8sLogBugCheck(_In_ PVOID Buffer, _In_ ULONG Length)
{
    DEVICE_CONTEXT *Ctx = gCtx;
    char           Line[160], *P, *End = Line + sizeof(Line) - 1;
    int            i;

    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);

    if (Ctx == NULL) {
        return;
    }

    /* HIGH_LEVEL, one processor running: no locks. */
    P = PutStr(Line, End, "\r\n[r8slog] BUGCHECK at t=");
    P = PutDec(P, End, KeQueryInterruptTime() / 10000000ULL);
    P = PutStr(P, End, " s");
    if (gBugCheckData != NULL) {
        P = PutStr(P, End, ": code ");
        P = PutHex(P, End, gBugCheckData[0], 8);
        for (i = 1; i <= 4; i++) {
            P = PutStr(P, End, " ");
            P = PutHex(P, End, gBugCheckData[i], 16);
        }
    }

    P = PutStr(P, End, "\r\n");
    *P = '\0';
    AppendLine(Ctx, Line, FALSE);
}

VOID
R8sLogWrite(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t Length)
{
    DEVICE_CONTEXT *Ctx = GetContext(WdfIoQueueGetDevice(Queue));
    NTSTATUS       Status;
    PVOID          Buffer;
    size_t         Size;

    if (Length == 0) {
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0);
        return;
    }

    Status = WdfRequestRetrieveInputBuffer(Request, 1, &Buffer, &Size);
    if (!NT_SUCCESS(Status) || (Ctx->Base == NULL)) {
        WdfRequestComplete(Request, NT_SUCCESS(Status) ? STATUS_DEVICE_NOT_READY : Status);
        return;
    }

    WdfSpinLockAcquire(Ctx->Lock);
    AppendRaw(Ctx, (const UCHAR *)Buffer, Size);
    WdfSpinLockRelease(Ctx->Lock);

    /* Report everything as written so cmd.exe redirection never errors when full. */
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, Size);
}

VOID
R8sLogRead(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t Length)
{
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(Length);

    /* Nothing to read back; EOF. */
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0);
}
