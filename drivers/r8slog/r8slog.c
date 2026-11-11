/*
 * r8slog.sys - r8s-woa Windows -> firmware log handoff (KMDF).
 *
 * Binds to ACPI\RWOA0002 (DSDT \_SB.WLOG), whose _CRS is a reserved RAM
 * region the bootloader keeps across warm resets. Anything written to
 * \\.\R8sLog (e.g. "pnputil /enum-devices > \\.\R8sLog") is appended there.
 * After Windows reboots, the firmware's R8sDiag.efi saves the text to
 * \r8s-winlog.txt on the ESP.
 *
 * Layout (shared with R8sDiag.c): 64-byte header, then text.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <ntddk.h>
#include <wdf.h>

#define WINLOG_MAGIC  0x31474F4C57533852ULL      /* "R8SWLOG1" */

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
    WDFSPINLOCK     Lock;
} DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetContext)

DRIVER_INITIALIZE                   DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD           R8sLogDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE     R8sLogPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE     R8sLogReleaseHardware;
EVT_WDF_IO_QUEUE_IO_WRITE           R8sLogWrite;
EVT_WDF_IO_QUEUE_IO_READ            R8sLogRead;

static void
AppendLocked(DEVICE_CONTEXT *Ctx, const UCHAR *Data, SIZE_T Len)
{
    WINLOG_HEADER *Hdr = (WINLOG_HEADER *)Ctx->Base;
    SIZE_T Room, i;

    if (Hdr->Length > Hdr->Capacity) {
        return;
    }

    Room = Hdr->Capacity - Hdr->Length;
    if (Len > Room) {
        Len = Room;
    }

    /* Device (non-cached) memory: byte stores, no memcpy alignment tricks. */
    for (i = 0; i < Len; i++) {
        Ctx->Base[sizeof(WINLOG_HEADER) + Hdr->Length + i] = Data[i];
    }

    Hdr->Length += (ULONG)Len;
    Hdr->Writes++;
}

static void
AppendString(DEVICE_CONTEXT *Ctx, const char *Text)
{
    SIZE_T Len = 0;
    while (Text[Len] != '\0') {
        Len++;
    }

    WdfSpinLockAcquire(Ctx->Lock);
    AppendLocked(Ctx, (const UCHAR *)Text, Len);
    WdfSpinLockRelease(Ctx->Lock);
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

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
    Ctx->Base = NULL;
    Ctx->Size = 0;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attr);
    Attr.ParentObject = Device;
    Status = WdfSpinLockCreate(&Attr, &Ctx->Lock);
    if (!NT_SUCCESS(Status)) {
        return Status;
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
    WINLOG_HEADER                   *Hdr;

    UNREFERENCED_PARAMETER(Raw);

    Count = WdfCmResourceListGetCount(Translated);
    for (i = 0; i < Count; i++) {
        Desc = WdfCmResourceListGetDescriptor(Translated, i);
        if ((Desc != NULL) && (Desc->Type == CmResourceTypeMemory) && (Desc->u.Memory.Length > sizeof(WINLOG_HEADER))) {
            Ctx->Size = Desc->u.Memory.Length;
            Ctx->Base = (volatile UCHAR *)MmMapIoSpaceEx(Desc->u.Memory.Start, Ctx->Size, PAGE_READWRITE | PAGE_NOCACHE);
            break;
        }
    }

    if (Ctx->Base == NULL) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* Keep text from an earlier boot the firmware hasn't saved yet. */
    Hdr = (WINLOG_HEADER *)Ctx->Base;
    if ((Hdr->Magic != WINLOG_MAGIC) || (Hdr->Capacity != Ctx->Size - sizeof(WINLOG_HEADER)) || (Hdr->Length > Hdr->Capacity)) {
        Hdr->Length   = 0;
        Hdr->Writes   = 0;
        Hdr->Capacity = (ULONG)(Ctx->Size - sizeof(WINLOG_HEADER));
        Hdr->Magic    = WINLOG_MAGIC;
    }

    AppendString(Ctx, "\r\n[r8slog] driver started, log RAM mapped\r\n");
    return STATUS_SUCCESS;
}

NTSTATUS
R8sLogReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    DEVICE_CONTEXT *Ctx = GetContext(Device);

    UNREFERENCED_PARAMETER(Translated);

    if (Ctx->Base != NULL) {
        MmUnmapIoSpace((PVOID)Ctx->Base, Ctx->Size);
        Ctx->Base = NULL;
    }

    return STATUS_SUCCESS;
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
    AppendLocked(Ctx, (const UCHAR *)Buffer, Size);
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
