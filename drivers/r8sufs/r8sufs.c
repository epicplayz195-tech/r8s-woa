/*
 * r8sufs.sys - Galaxy S20 FE (r8s, Exynos 990) UFS storage for Windows (Storport miniport).
 *
 * The controller at 0x13100000 is UFSHCI with Samsung's quirks (Linux ufs-exynos.c; Silicium
 * SamsungPkg UfsDxe; r8sios redwood_ufshc.c):
 *   - the request descriptor's response/PRDT offsets and lengths are in BYTES, not dwords;
 *   - a PRD entry covers at most 4 KB (vendor TX/RXPRDT_ENTRY_SIZE = 12), size field = bytes - 1;
 *   - a slot runs a SCSI command only if its bit in the vendor UTRL_NEXUS_TYPE (+0x1140) is set;
 *   - UTRLCLR clears a slot by writing a ONE;
 *   - DMA is cache coherent (DT: dma-coherent; ACPI _CCA 1).
 *
 * Like the r8sios driver, nothing of the link is touched: no reset, no link startup, no UIC
 * command, no power or clock change. UEFI's UfsDxe brought the link and device up; at start this
 * driver stops the transfer list, points it at its own (one slot used, the rest empty), and starts
 * it again. Interrupts stay off: every command is rung and polled to completion in HwStartIo, one
 * at a time, through a bounce buffer in the uncached extension (the approach of the Galaxy S9+
 * project's Exynos9810Ufs driver).
 *
 * Only UFS LUN 0 (the main user LUN, holding the GPT with esp/win) is exposed. Commands are
 * allow-listed. WRITEs are allowed only inside the GPT partitions named "esp" and "win"; the
 * table is read on the first command. Anything else that could change the device (FORMAT, WRITE
 * BUFFER, MODE SELECT, SECURITY PROTOCOL OUT, writes elsewhere) is refused with ILLEGAL REQUEST /
 * DATA PROTECT. START STOP UNIT, VERIFY and UNMAP are answered without reaching the device.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <ntddk.h>
#include <storport.h>
#include <scsi.h>

/* ---------------------------------------------------------------- controller */

#define HCI_SIZE            0x2000

#define REG_CAP             0x00
#define REG_IS              0x20
#define REG_IE              0x24
#define REG_HCS             0x30
#define REG_HCE             0x34
#define REG_UTRLBA          0x50
#define REG_UTRLBAU         0x54
#define REG_UTRLDBR         0x58
#define REG_UTRLCLR         0x5C
#define REG_UTRLRSR         0x60
#define VS_TXPRDT_SIZE      0x1100
#define VS_RXPRDT_SIZE      0x1104
#define VS_UTRL_NEXUS       0x1140

#define HCS_UTRLRDY         (1u << 1)
#define IS_FATAL            ((1u << 11) | (1u << 12) | (1u << 16) | (1u << 17))     /* DFES, UTPES, HCFES, SBFES */

#define UTRD_DD_NONE        0x00000000u
#define UTRD_DD_OUT         0x02000000u     /* host to device */
#define UTRD_DD_IN          0x04000000u     /* device to host */
#define UTRD_INT            0x01000000u
#define OCS_SUCCESS         0x0
#define OCS_INVALID         0xF

#define UPIU_COMMAND        0x01
#define UPIU_RESPONSE       0x21
#define UPIU_FLAG_WRITE     0x20
#define UPIU_FLAG_READ      0x40

#define UPIU_SIZE           1024            /* command and response UPIU slots (Silicium ALIGNED_UPIU_SIZE) */
#define PRD_BYTES           4096
#define MAX_TRANSFER        (256 * 1024)
#define MAX_PRD             (MAX_TRANSFER / PRD_BYTES)

#define POLL_US             5
#define TIMEOUT_US          10000000        /* 10 s per command */

#pragma pack(push, 1)
typedef struct _UTRD {
    ULONG  Dw0;
    ULONG  Dw1;
    ULONG  Dw2;                 /* OCS in bits 7:0 */
    ULONG  Dw3;
    ULONG  UcdLow;
    ULONG  UcdHigh;
    USHORT RspLen;              /* bytes (quirk) */
    USHORT RspOff;
    USHORT PrdtLen;             /* bytes (quirk) */
    USHORT PrdtOff;
} UTRD;

typedef struct _PRD {
    ULONG Base;
    ULONG Upper;
    ULONG Reserved;
    ULONG Size;                 /* bytes - 1 */
} PRD;

typedef struct _UCD {
    UCHAR Command[UPIU_SIZE];
    UCHAR Response[UPIU_SIZE];
    PRD   Prdt[MAX_PRD];
} UCD;
#pragma pack(pop)

/* Uncached extension layout (physically contiguous). */
#define OFF_UTRL            0x0000          /* 32 x 32 bytes, 1 KB aligned */
#define OFF_UCD             0x1000          /* 128-byte aligned */
#define OFF_BOUNCE          0x2000          /* 4 KB aligned */
#define UNCACHED_SIZE       (OFF_BOUNCE + MAX_TRANSFER + 0x1000)

/* ---------------------------------------------------------------- SCSI */

#define OP_TEST_UNIT_READY  0x00
#define OP_REQUEST_SENSE    0x03
#define OP_READ6            0x08
#define OP_INQUIRY          0x12
#define OP_MODE_SENSE6      0x1A
#define OP_START_STOP       0x1B
#define OP_READ_CAPACITY10  0x25
#define OP_READ10           0x28
#define OP_WRITE10          0x2A
#define OP_VERIFY10         0x2F
#define OP_SYNC_CACHE10     0x35
#define OP_UNMAP            0x42
#define OP_MODE_SENSE10     0x5A
#define OP_READ16           0x88
#define OP_WRITE16          0x8A
#define OP_VERIFY16         0x8F
#define OP_SYNC_CACHE16     0x91
#define OP_SERVICE_IN16     0x9E            /* READ CAPACITY(16) */
#define OP_REPORT_LUNS      0xA0

#define MAX_RANGES          4

typedef struct _RANGE {
    ULONGLONG First, Last;      /* inclusive, logical blocks */
} RANGE;

typedef struct _ADAPTER {
    PUCHAR    Hci;
    PUCHAR    Uncached;
    ULONGLONG UncachedPhys;
    UTRD     *Utrd;             /* slot 0 */
    UCD      *Ucd;
    PUCHAR    Bounce;
    ULONGLONG UcdPhys, BouncePhys, UtrlPhys;
    BOOLEAN   Ready;            /* our list is in the controller */
    BOOLEAN   Fatal;
    BOOLEAN   GptRead;
    ULONG     BlockSize;
    ULONG     RangeCount;
    RANGE     Range[MAX_RANGES];
    UCHAR     LastResponse[64];
} ADAPTER, *PADAPTER;

static ULONG Rd(PADAPTER A, ULONG Off) { return StorPortReadRegisterUlong(A, (PULONG)(A->Hci + Off)); }
static VOID  Wr(PADAPTER A, ULONG Off, ULONG V) { StorPortWriteRegisterUlong(A, (PULONG)(A->Hci + Off), V); }

static ULONG Be32(const UCHAR *P) { return ((ULONG)P[0] << 24) | ((ULONG)P[1] << 16) | ((ULONG)P[2] << 8) | P[3]; }
static USHORT Be16(const UCHAR *P) { return (USHORT)((P[0] << 8) | P[1]); }
static ULONGLONG Be64(const UCHAR *P) { return ((ULONGLONG)Be32(P) << 32) | Be32(P + 4); }
static ULONGLONG Le64(const UCHAR *P) { ULONGLONG v = 0; int i; for (i = 7; i >= 0; i--) { v = (v << 8) | P[i]; } return v; }
static ULONG Le32(const UCHAR *P) { return (ULONG)P[0] | ((ULONG)P[1] << 8) | ((ULONG)P[2] << 16) | ((ULONG)P[3] << 24); }

/* ---------------------------------------------------------------- takeover */

static BOOLEAN
TakeOver(PADAPTER A)
{
    ULONG i;

    A->Ready = FALSE;
    if (Rd(A, REG_HCE) == 0 || (Rd(A, REG_HCS) & HCS_UTRLRDY) == 0) {
        return FALSE;           /* UEFI did not leave the controller up: nothing we can do */
    }
    for (i = 0; i < 100000 / POLL_US && Rd(A, REG_UTRLDBR) != 0; i++) {
        StorPortStallExecution(POLL_US);
    }
    if (Rd(A, REG_UTRLDBR) != 0) {
        return FALSE;
    }

    RtlZeroMemory(A->Uncached + OFF_UTRL, 32 * sizeof(UTRD));
    Wr(A, REG_IE, 0);                       /* polled */
    Wr(A, REG_UTRLRSR, 0);
    StorPortStallExecution(100);
    Wr(A, REG_UTRLBA, (ULONG)A->UtrlPhys);
    Wr(A, REG_UTRLBAU, (ULONG)(A->UtrlPhys >> 32));
    Wr(A, VS_TXPRDT_SIZE, 12);
    Wr(A, VS_RXPRDT_SIZE, 12);
    Wr(A, VS_UTRL_NEXUS, 0xFFFFFFFF);
    Wr(A, REG_UTRLRSR, 1);
    Wr(A, REG_IS, Rd(A, REG_IS));           /* clear what UEFI left */
    A->Ready = (Rd(A, REG_UTRLBA) == (ULONG)A->UtrlPhys) && (Rd(A, REG_UTRLRSR) & 1);
    return A->Ready;
}

/* ---------------------------------------------------------------- one command */

typedef enum { EXEC_OK, EXEC_CHECK, EXEC_FAILED, EXEC_TIMEOUT } EXEC_RESULT;

/* Run one SCSI command on LUN 0 through the bounce buffer. In: for writes, data already in
 * Bounce. Out: ScsiStatus, Residual, sense copied into A->LastResponse (sense at +2). */
static EXEC_RESULT
Exec(PADAPTER A, const UCHAR *Cdb, ULONG CdbLen, ULONG Dir, ULONG Length, PUCHAR ScsiStatus, PULONG Residual)
{
    UTRD *t = A->Utrd;
    UCD *u = A->Ucd;
    ULONG i, n, is;
    USHORT segLen;

    *ScsiStatus = 0;
    *Residual = 0;
    if (!A->Ready || A->Fatal || Length > MAX_TRANSFER || CdbLen > 16) {
        return EXEC_FAILED;
    }

    RtlZeroMemory(u->Command, sizeof(u->Command));
    RtlZeroMemory(u->Response, 64);
    u->Command[0] = UPIU_COMMAND;
    u->Command[1] = (UCHAR)(Dir == UTRD_DD_IN ? UPIU_FLAG_READ : Dir == UTRD_DD_OUT ? UPIU_FLAG_WRITE : 0);
    u->Command[2] = 0;                      /* LUN 0 */
    u->Command[3] = 0;                      /* task tag */
    u->Command[12] = (UCHAR)(Length >> 24);
    u->Command[13] = (UCHAR)(Length >> 16);
    u->Command[14] = (UCHAR)(Length >> 8);
    u->Command[15] = (UCHAR)Length;
    RtlCopyMemory(&u->Command[16], Cdb, CdbLen);

    n = (Length + PRD_BYTES - 1) / PRD_BYTES;
    for (i = 0; i < n; i++) {
        ULONGLONG pa = A->BouncePhys + (ULONGLONG)i * PRD_BYTES;
        ULONG left = Length - i * PRD_BYTES;

        u->Prdt[i].Base = (ULONG)pa;
        u->Prdt[i].Upper = (ULONG)(pa >> 32);
        u->Prdt[i].Reserved = 0;
        u->Prdt[i].Size = (left < PRD_BYTES ? left : PRD_BYTES) - 1;
    }

    RtlZeroMemory(t, sizeof(*t));
    t->Dw0 = Dir | UTRD_INT;                /* command type 0 = SCSI, as UfsDxe */
    t->Dw2 = OCS_INVALID;
    t->UcdLow = (ULONG)A->UcdPhys;
    t->UcdHigh = (ULONG)(A->UcdPhys >> 32);
    t->RspOff = UPIU_SIZE;
    t->RspLen = UPIU_SIZE;
    t->PrdtOff = n ? 2 * UPIU_SIZE : 0;
    t->PrdtLen = (USHORT)(n * sizeof(PRD));
    MemoryBarrier();

    Wr(A, VS_UTRL_NEXUS, 0xFFFFFFFF);
    Wr(A, REG_UTRLDBR, 1);
    for (i = 0; i < TIMEOUT_US / POLL_US; i++) {
        if ((Rd(A, REG_UTRLDBR) & 1) == 0) {
            break;
        }
        is = Rd(A, REG_IS);
        if (is & IS_FATAL) {
            A->Fatal = TRUE;
            break;
        }
        StorPortStallExecution(POLL_US);
    }
    Wr(A, REG_IS, Rd(A, REG_IS));
    if (A->Fatal || (Rd(A, REG_UTRLDBR) & 1)) {
        Wr(A, REG_UTRLCLR, 1);              /* quirk: one clears */
        return A->Fatal ? EXEC_FAILED : EXEC_TIMEOUT;
    }
    MemoryBarrier();

    RtlCopyMemory(A->LastResponse, u->Response, sizeof(A->LastResponse));
    if ((t->Dw2 & 0xF) != OCS_SUCCESS || u->Response[0] != UPIU_RESPONSE || u->Response[6] != 0) {
        return EXEC_FAILED;
    }
    *ScsiStatus = u->Response[7];
    *Residual = Be32(&u->Response[12]);
    segLen = Be16(&u->Response[10]);
    UNREFERENCED_PARAMETER(segLen);
    return *ScsiStatus == SCSISTAT_GOOD ? EXEC_OK : EXEC_CHECK;
}

/* Internal read of Blocks blocks at Lba into the bounce buffer. */
static BOOLEAN
ReadBlocks(PADAPTER A, ULONGLONG Lba, ULONG Blocks)
{
    UCHAR cdb[10] = { OP_READ10, 0, (UCHAR)(Lba >> 24), (UCHAR)(Lba >> 16), (UCHAR)(Lba >> 8), (UCHAR)Lba,
                      0, (UCHAR)(Blocks >> 8), (UCHAR)Blocks, 0 };
    UCHAR st;
    ULONG res;

    return Exec(A, cdb, 10, UTRD_DD_IN, Blocks * A->BlockSize, &st, &res) == EXEC_OK;
}

/* Find the esp and win partitions: the only places writes may go. */
static VOID
ReadGpt(PADAPTER A)
{
    UCHAR cdb[10] = { OP_READ_CAPACITY10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    UCHAR st;
    ULONG res, i, count, size, blocks;
    ULONGLONG entLba;

    A->GptRead = TRUE;
    A->RangeCount = 0;
    if (Exec(A, cdb, 10, UTRD_DD_IN, 8, &st, &res) != EXEC_OK) {
        return;
    }
    A->BlockSize = Be32(A->Bounce + 4);
    if (A->BlockSize != 512 && A->BlockSize != 4096) {
        return;
    }
    if (!ReadBlocks(A, 1, 1) || RtlCompareMemory(A->Bounce, "EFI PART", 8) != 8) {
        return;
    }
    entLba = Le64(A->Bounce + 72);
    count = Le32(A->Bounce + 80);
    size = Le32(A->Bounce + 84);
    if (size != 128 || count == 0 || count > 256) {
        return;
    }
    blocks = (count * size + A->BlockSize - 1) / A->BlockSize;
    if (blocks * A->BlockSize > MAX_TRANSFER || !ReadBlocks(A, entLba, blocks)) {
        return;
    }
    for (i = 0; i < count && A->RangeCount < MAX_RANGES; i++) {
        const UCHAR *e = A->Bounce + i * size;
        const UCHAR *name = e + 56;         /* UTF-16LE */
        BOOLEAN esp = name[0] == 'e' && name[1] == 0 && name[2] == 's' && name[3] == 0 && name[4] == 'p' && name[5] == 0 && name[6] == 0 && name[7] == 0;
        BOOLEAN win = name[0] == 'w' && name[1] == 0 && name[2] == 'i' && name[3] == 0 && name[4] == 'n' && name[5] == 0 && name[6] == 0 && name[7] == 0;

        if (esp || win) {
            A->Range[A->RangeCount].First = Le64(e + 32);
            A->Range[A->RangeCount].Last = Le64(e + 40);
            A->RangeCount++;
        }
    }
}

static BOOLEAN
WriteAllowed(PADAPTER A, ULONGLONG Lba, ULONGLONG Blocks)
{
    ULONG i;

    if (Blocks == 0) {
        return TRUE;
    }
    for (i = 0; i < A->RangeCount; i++) {
        if (Lba >= A->Range[i].First && Lba + Blocks - 1 <= A->Range[i].Last) {
            return TRUE;
        }
    }
    return FALSE;
}

/* ---------------------------------------------------------------- SRBs */

static VOID
SetSense(PSCSI_REQUEST_BLOCK Srb, UCHAR Key, UCHAR Asc, UCHAR Ascq)
{
    PSENSE_DATA s = (PSENSE_DATA)Srb->SenseInfoBuffer;

    Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
    Srb->SrbStatus = SRB_STATUS_ERROR;
    if (s != NULL && Srb->SenseInfoBufferLength >= sizeof(SENSE_DATA)) {
        RtlZeroMemory(s, sizeof(SENSE_DATA));
        s->ErrorCode = SCSI_SENSE_ERRORCODE_FIXED_CURRENT;
        s->SenseKey = Key;
        s->AdditionalSenseLength = sizeof(SENSE_DATA) - 8;
        s->AdditionalSenseCode = Asc;
        s->AdditionalSenseCodeQualifier = Ascq;
        Srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
    }
}

static VOID
DoScsi(PADAPTER A, PSCSI_REQUEST_BLOCK Srb)
{
    const UCHAR *cdb = Srb->Cdb;
    ULONG len = Srb->DataTransferLength;
    ULONG dir = UTRD_DD_NONE;
    UCHAR st;
    ULONG res;
    EXEC_RESULT r;

    if (Srb->PathId != 0 || Srb->TargetId != 0 || Srb->Lun != 0) {
        Srb->SrbStatus = SRB_STATUS_SELECTION_TIMEOUT;
        return;
    }
    if (!A->GptRead) {
        ReadGpt(A);
    }

    switch (cdb[0]) {
    case OP_TEST_UNIT_READY:
    case OP_REQUEST_SENSE:
    case OP_INQUIRY:
    case OP_MODE_SENSE6:
    case OP_MODE_SENSE10:
    case OP_READ_CAPACITY10:
    case OP_SERVICE_IN16:
    case OP_READ6:
    case OP_READ10:
    case OP_READ16:
    case OP_SYNC_CACHE10:
    case OP_SYNC_CACHE16:
        break;

    case OP_WRITE10:
    case OP_WRITE16: {
        ULONGLONG lba = cdb[0] == OP_WRITE10 ? Be32(cdb + 2) : Be64(cdb + 2);
        ULONGLONG blocks = cdb[0] == OP_WRITE10 ? Be16(cdb + 7) : Be32(cdb + 10);

        if (!WriteAllowed(A, lba, blocks)) {
            SetSense(Srb, SCSI_SENSE_DATA_PROTECT, SCSI_ADSENSE_WRITE_PROTECT, 0);
            return;
        }
        break;
    }

    case OP_START_STOP:                     /* never stop the device: the firmware could not restart it */
    case OP_VERIFY10:
    case OP_VERIFY16:
    case OP_UNMAP:                          /* no TRIM (yet): harmless to skip */
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        Srb->ScsiStatus = SCSISTAT_GOOD;
        return;

    case OP_REPORT_LUNS:                    /* LUN 0 only */
        if (len >= 16) {
            PUCHAR p = (PUCHAR)Srb->DataBuffer;
            RtlZeroMemory(p, 16);
            p[3] = 8;                       /* one LUN, 8 bytes */
            Srb->DataTransferLength = 16;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
        } else {
            Srb->SrbStatus = SRB_STATUS_DATA_OVERRUN;
        }
        return;

    default:
        SetSense(Srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_ILLEGAL_COMMAND, 0);
        return;
    }

    if (len > MAX_TRANSFER) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        return;
    }
    if (len != 0) {
        if (Srb->SrbFlags & SRB_FLAGS_DATA_IN) {
            dir = UTRD_DD_IN;
        } else if (Srb->SrbFlags & SRB_FLAGS_DATA_OUT) {
            dir = UTRD_DD_OUT;
            RtlCopyMemory(A->Bounce, Srb->DataBuffer, len);
        }
    }

    r = Exec(A, cdb, Srb->CdbLength, dir, len, &st, &res);
    switch (r) {
    case EXEC_OK:
        if (dir == UTRD_DD_IN) {
            ULONG got = res < len ? len - res : 0;
            RtlCopyMemory(Srb->DataBuffer, A->Bounce, got);
            if (got < len) {
                Srb->DataTransferLength = got;
                Srb->SrbStatus = SRB_STATUS_DATA_OVERRUN;   /* = underrun for Storport */
                Srb->ScsiStatus = SCSISTAT_GOOD;
                return;
            }
        }
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        Srb->ScsiStatus = SCSISTAT_GOOD;
        return;

    case EXEC_CHECK:
        Srb->ScsiStatus = st;
        Srb->SrbStatus = SRB_STATUS_ERROR;
        if (st == SCSISTAT_CHECK_CONDITION && Srb->SenseInfoBuffer != NULL && Srb->SenseInfoBufferLength != 0) {
            USHORT senseLen = Be16(&A->LastResponse[32]);       /* data segment: length, then sense */
            ULONG copy = min((ULONG)senseLen, (ULONG)Srb->SenseInfoBufferLength);

            copy = min(copy, (ULONG)(sizeof(A->LastResponse) - 34));
            if (copy != 0) {
                RtlCopyMemory(Srb->SenseInfoBuffer, &A->Ucd->Response[34], copy);
                Srb->SenseInfoBufferLength = (UCHAR)copy;
                Srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
            }
        }
        return;

    case EXEC_TIMEOUT:
        Srb->SrbStatus = SRB_STATUS_TIMEOUT;
        return;

    default:
        Srb->SrbStatus = SRB_STATUS_ERROR;
        return;
    }
}

/* ---------------------------------------------------------------- Storport entry points */

BOOLEAN
HwStartIo(PVOID DeviceExtension, PSCSI_REQUEST_BLOCK Srb)
{
    PADAPTER a = (PADAPTER)DeviceExtension;

    switch (Srb->Function) {
    case SRB_FUNCTION_EXECUTE_SCSI:
        DoScsi(a, Srb);
        break;
    case SRB_FUNCTION_FLUSH:
    case SRB_FUNCTION_SHUTDOWN: {
        UCHAR cdb[10] = { OP_SYNC_CACHE10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        UCHAR st;
        ULONG res;

        (VOID)Exec(a, cdb, 10, UTRD_DD_NONE, 0, &st, &res);
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;
    }
    case SRB_FUNCTION_RESET_BUS:
    case SRB_FUNCTION_RESET_DEVICE:
    case SRB_FUNCTION_RESET_LOGICAL_UNIT:
    case SRB_FUNCTION_PNP:
    case SRB_FUNCTION_POWER:
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;
    default:
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    }
    StorPortNotification(RequestComplete, DeviceExtension, Srb);
    return TRUE;
}

BOOLEAN
HwInterrupt(PVOID DeviceExtension)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    return FALSE;                           /* polled */
}

BOOLEAN
HwResetBus(PVOID DeviceExtension, ULONG PathId)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(PathId);
    return TRUE;
}

BOOLEAN
HwInitialize(PVOID DeviceExtension)
{
    return TakeOver((PADAPTER)DeviceExtension);
}

SCSI_ADAPTER_CONTROL_STATUS
HwAdapterControl(PVOID DeviceExtension, SCSI_ADAPTER_CONTROL_TYPE Type, PVOID Parameters)
{
    PSCSI_SUPPORTED_CONTROL_TYPE_LIST list;

    switch (Type) {
    case ScsiQuerySupportedControlTypes:
        list = (PSCSI_SUPPORTED_CONTROL_TYPE_LIST)Parameters;
        if (list->MaxControlType > ScsiStopAdapter) { list->SupportedTypeList[ScsiStopAdapter] = TRUE; }
        if (list->MaxControlType > ScsiRestartAdapter) { list->SupportedTypeList[ScsiRestartAdapter] = TRUE; }
        if (list->MaxControlType > ScsiQuerySupportedControlTypes) { list->SupportedTypeList[ScsiQuerySupportedControlTypes] = TRUE; }
        return ScsiAdapterControlSuccess;
    case ScsiStopAdapter:
        return ScsiAdapterControlSuccess;   /* leave the link as it is */
    case ScsiRestartAdapter:
        return TakeOver((PADAPTER)DeviceExtension) ? ScsiAdapterControlSuccess : ScsiAdapterControlUnsuccessful;
    default:
        return ScsiAdapterControlUnsuccessful;
    }
}

ULONG
HwFindAdapter(PVOID DeviceExtension, PVOID HwContext, PVOID BusInformation, PCHAR ArgumentString,
              PPORT_CONFIGURATION_INFORMATION ConfigInfo, PBOOLEAN Again)
{
    PADAPTER a = (PADAPTER)DeviceExtension;
    PACCESS_RANGE r;
    STOR_PHYSICAL_ADDRESS pa;
    ULONG len;

    UNREFERENCED_PARAMETER(HwContext);
    UNREFERENCED_PARAMETER(BusInformation);
    UNREFERENCED_PARAMETER(ArgumentString);
    *Again = FALSE;

    if (ConfigInfo->NumberOfAccessRanges < 1 || ConfigInfo->AccessRanges == NULL) {
        return SP_RETURN_NOT_FOUND;
    }
    r = &(*ConfigInfo->AccessRanges)[0];
    if (r->RangeLength < HCI_SIZE) {
        return SP_RETURN_BAD_CONFIG;
    }
    a->Hci = (PUCHAR)StorPortGetDeviceBase(a, ConfigInfo->AdapterInterfaceType, ConfigInfo->SystemIoBusNumber,
                                           r->RangeStart, HCI_SIZE, FALSE);
    if (a->Hci == NULL) {
        return SP_RETURN_ERROR;
    }

    ConfigInfo->MaximumTransferLength = MAX_TRANSFER;
    ConfigInfo->NumberOfPhysicalBreaks = MAX_PRD;
    ConfigInfo->AlignmentMask = 3;
    ConfigInfo->ScatterGather = TRUE;
    ConfigInfo->Master = TRUE;
    ConfigInfo->CachesData = FALSE;
    ConfigInfo->NumberOfBuses = 1;
    ConfigInfo->MaximumNumberOfTargets = 1;
    ConfigInfo->MaximumNumberOfLogicalUnits = 1;
    ConfigInfo->MapBuffers = STOR_MAP_ALL_BUFFERS_INCLUDING_READ_WRITE;
    ConfigInfo->Dma64BitAddresses = 0;
    ConfigInfo->SynchronizationModel = StorSynchronizeFullDuplex;
    ConfigInfo->InterruptSynchronizationMode = InterruptSupportNone;
    ConfigInfo->MaxIOsPerLun = 1;
    ConfigInfo->InitialLunQueueDepth = 1;

    a->Uncached = (PUCHAR)StorPortGetUncachedExtension(a, ConfigInfo, UNCACHED_SIZE);
    if (a->Uncached == NULL) {
        return SP_RETURN_ERROR;
    }
    pa = StorPortGetPhysicalAddress(a, NULL, a->Uncached, &len);
    a->UncachedPhys = (ULONGLONG)pa.QuadPart;
    if (len < UNCACHED_SIZE || (a->UncachedPhys & 0xFFF) != 0) {
        /* not one contiguous, page-aligned block: the offsets below would not hold */
        return SP_RETURN_ERROR;
    }
    RtlZeroMemory(a->Uncached, UNCACHED_SIZE);
    a->UtrlPhys = a->UncachedPhys + OFF_UTRL;
    a->UcdPhys = a->UncachedPhys + OFF_UCD;
    a->BouncePhys = a->UncachedPhys + OFF_BOUNCE;
    a->Utrd = (UTRD *)(a->Uncached + OFF_UTRL);
    a->Ucd = (UCD *)(a->Uncached + OFF_UCD);
    a->Bounce = a->Uncached + OFF_BOUNCE;
    a->BlockSize = 4096;
    return SP_RETURN_FOUND;
}

ULONG
DriverEntry(PVOID DriverObject, PVOID RegistryPath)
{
    HW_INITIALIZATION_DATA init;

    C_ASSERT(sizeof(UTRD) == 32);
    C_ASSERT(sizeof(PRD) == 16);
    C_ASSERT(OFF_UCD + sizeof(UCD) <= OFF_BOUNCE);

    RtlZeroMemory(&init, sizeof(init));
    init.HwInitializationDataSize = sizeof(init);
    init.AdapterInterfaceType = InterfaceTypeUndefined;
    init.HwInitialize = HwInitialize;
    init.HwStartIo = HwStartIo;
    init.HwInterrupt = HwInterrupt;
    init.HwFindAdapter = HwFindAdapter;
    init.HwResetBus = HwResetBus;
    init.HwAdapterControl = HwAdapterControl;
    init.DeviceExtensionSize = sizeof(ADAPTER);
    init.NumberOfAccessRanges = 1;
    init.MapBuffers = STOR_MAP_ALL_BUFFERS_INCLUDING_READ_WRITE;
    init.TaggedQueuing = FALSE;
    init.AutoRequestSense = TRUE;
    init.MultipleRequestPerLu = FALSE;
    init.NeedPhysicalAddresses = TRUE;
    return StorPortInitialize(DriverObject, RegistryPath, &init, NULL);
}
