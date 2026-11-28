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
#include <ntstrsafe.h>

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

typedef enum { R_OK, R_UNDERRUN, R_CHECK, R_TIMEOUT, R_FAILED, R_REFUSED, R_LOCAL } LOG_RESULT;

#define LOG_ALL_FIRST       600             /* every command until then, afterwards only errors */

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
    UCHAR     LastOcs;
    volatile UCHAR *Inbox;          /* r8slog inbox (see Say), NULL if it could not be mapped */
    ULONG     Seq;
    ULONG     Errors;
    ULONG     LastResidual;
    ULONG     GptEntriesCrc;
    ULONG     GptFixes;
    BOOLEAN   GptHide;              /* entries are presented with the Android partitions hidden */
    ULONG     GptHiddenCrc;         /* entries CRC with that done */
    ULONG     GptEntrySize, GptEntryCount;
    ULONGLONG GptEntLba, GptAltEntLba, GptEntBlocks;
    ULONG     LastIs;
} ADAPTER, *PADAPTER;

/*
 * Log inbox: the last 16 KB of the r8slog RAM region (DSDT WLOG 0xFDB10000, 4 MB). r8slog uses
 * 5 page-aligned slices of 835584 bytes = 4177920 bytes, so 0xFDF0C000..0xFDF0FFFF is free.
 * r8sufs appends text to a ring there with plain stores (any IRQL); r8slog's 5 s timer copies
 * new text into the log the firmware saves. No files, no work items, no registry.
 * Header: Magic, Check (= Magic ^ Gen low ^ Gen high), Gen (interrupt time at start), Head
 * (total bytes ever written; ring position = Head % INBOX_DATA).
 */
#define INBOX_PHYS          0xFDF0C000ULL
#define INBOX_SIZE          0x4000
#define INBOX_HDR           64
#define INBOX_DATA          (INBOX_SIZE - INBOX_HDR)
#define INBOX_MAGIC         0x49534655u     /* "UFSI" */

static ULONG Rd(PADAPTER A, ULONG Off) { return StorPortReadRegisterUlong(A, (PULONG)(A->Hci + Off)); }
static VOID  Wr(PADAPTER A, ULONG Off, ULONG V) { StorPortWriteRegisterUlong(A, (PULONG)(A->Hci + Off), V); }

static ULONG Be32(const UCHAR *P) { return ((ULONG)P[0] << 24) | ((ULONG)P[1] << 16) | ((ULONG)P[2] << 8) | P[3]; }
static USHORT Be16(const UCHAR *P) { return (USHORT)((P[0] << 8) | P[1]); }
static ULONGLONG Be64(const UCHAR *P) { return ((ULONGLONG)Be32(P) << 32) | Be32(P + 4); }
static ULONGLONG Le64(const UCHAR *P) { ULONGLONG v = 0; int i; for (i = 7; i >= 0; i--) { v = (v << 8) | P[i]; } return v; }
static ULONG Le32(const UCHAR *P) { return (ULONG)P[0] | ((ULONG)P[1] << 8) | ((ULONG)P[2] << 16) | ((ULONG)P[3] << 24); }

/* Append a line to the r8slog inbox. Any IRQL; callers are serialised by Storport
 * (half duplex) except HwFindAdapter, which runs before any I/O. */
static VOID
Say(PADAPTER A, PCSTR Fmt, ...)
{
    CHAR line[256];
    va_list ap;
    size_t n = 0, i;
    volatile ULONG *hdr;
    ULONG head;

    if (A->Inbox == NULL) {
        return;
    }
    va_start(ap, Fmt);
    (VOID)RtlStringCbVPrintfA(line, sizeof(line), Fmt, ap);
    va_end(ap);
    if (!NT_SUCCESS(RtlStringCbLengthA(line, sizeof(line), &n))) {
        return;
    }
    hdr = (volatile ULONG *)A->Inbox;
    head = hdr[4];
    for (i = 0; i < n; i++) {
        A->Inbox[INBOX_HDR + (head + i) % INBOX_DATA] = (UCHAR)line[i];
    }
    MemoryBarrier();
    hdr[4] = head + (ULONG)n;
    MemoryBarrier();
}

static VOID
InboxStart(PADAPTER A)
{
    PHYSICAL_ADDRESS pa;
    volatile ULONG *hdr;
    ULONGLONG gen = KeQueryInterruptTime();

    pa.QuadPart = (LONGLONG)INBOX_PHYS;
    A->Inbox = (volatile UCHAR *)MmMapIoSpaceEx(pa, INBOX_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (A->Inbox == NULL) {
        return;
    }
    hdr = (volatile ULONG *)A->Inbox;
    hdr[0] = 0;                             /* invalid while being set up */
    MemoryBarrier();
    hdr[2] = (ULONG)gen;
    hdr[3] = (ULONG)(gen >> 32);
    hdr[4] = 0;
    hdr[1] = INBOX_MAGIC ^ (ULONG)gen ^ (ULONG)(gen >> 32);
    MemoryBarrier();
    hdr[0] = INBOX_MAGIC;
    MemoryBarrier();
    Say(A, "[ufs] r8sufs log inbox up, t=%I64u ms\r\n", gen / 10000);
}

/* Start-up notes for the log: HKLM\SYSTEM\CurrentControlSet\Services\r8sufs\Parameters\Device.
 * Only at PASSIVE_LEVEL (HwFindAdapter). */
static VOID Say(PADAPTER A, PCSTR Fmt, ...);

static VOID
Note(PADAPTER A, PCSTR Name, ULONG Value)
{
    ULONG len = sizeof(ULONG);
    PUCHAR buf;

    Say(A, "[ufs] %s = 0x%X\r\n", Name, Value);
    buf = StorPortAllocateRegistryBuffer(A, &len);

    if (buf == NULL) {
        return;
    }
    RtlCopyMemory(buf, &Value, sizeof(ULONG));
    (VOID)StorPortRegistryWrite(A, (PUCHAR)Name, TRUE, REG_DWORD, buf, sizeof(ULONG));
    StorPortFreeRegistryBuffer(A, buf);
}

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
    t->Dw0 = Dir;                           /* command type 0 = SCSI; no IOC: polled, the line stays quiet */
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
            Say(A, "[ufs] FATAL controller error, IS %08X, op %02X: no more commands\r\n", is, Cdb[0]);
            break;
        }
        StorPortStallExecution(POLL_US);
    }
    A->LastIs = Rd(A, REG_IS);
    Wr(A, REG_IS, A->LastIs);
    A->LastOcs = (UCHAR)(t->Dw2 & 0xF);
    A->LastResidual = 0;
    if (A->Fatal || (Rd(A, REG_UTRLDBR) & 1)) {
        Wr(A, REG_UTRLCLR, 1);              /* quirk: one clears */
        return A->Fatal ? EXEC_FAILED : EXEC_TIMEOUT;
    }
    MemoryBarrier();

    RtlCopyMemory(A->LastResponse, u->Response, sizeof(A->LastResponse));
    A->LastOcs = (UCHAR)(t->Dw2 & 0xF);
    if ((t->Dw2 & 0xF) != OCS_SUCCESS || u->Response[0] != UPIU_RESPONSE || u->Response[6] != 0) {
        return EXEC_FAILED;
    }
    *ScsiStatus = u->Response[7];
    *Residual = Be32(&u->Response[12]);
    A->LastResidual = *Residual;
    segLen = Be16(&u->Response[10]);
    UNREFERENCED_PARAMETER(segLen);
    return *ScsiStatus == SCSISTAT_GOOD ? EXEC_OK : EXEC_CHECK;
}


static ULONG
Crc32(const UCHAR *P, ULONG Len, ULONG Crc)
{
    ULONG i, b;

    Crc = ~Crc;
    for (i = 0; i < Len; i++) {
        Crc ^= P[i];
        for (b = 0; b < 8; b++) {
            Crc = (Crc >> 1) ^ (0xEDB88320u & (0u - (Crc & 1)));
        }
    }
    return ~Crc;
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
static BOOLEAN
NameIs(const UCHAR *E, const char *N)
{
    const UCHAR *name = E + 56;             /* UTF-16LE, 36 chars */
    ULONG i;

    for (i = 0; N[i]; i++) {
        if (name[2 * i] != (UCHAR)N[i] || name[2 * i + 1] != 0) {
            return FALSE;
        }
    }
    return name[2 * i] == 0 && name[2 * i + 1] == 0;
}

/*
 * Every Android partition is "basic data", so Windows mounts all ~30 of them and gives them
 * drive letters (and offers to format them). For Windows they are presented with the GPT
 * basic-data attributes read-only (60), hidden (62) and no drive letter (63). Only esp and win
 * stay as they are. Nothing on the device changes.
 */
#define GPT_ATTR_HIDE       ((1ULL << 60) | (1ULL << 62) | (1ULL << 63))

static VOID
HideEntries(PADAPTER A, PUCHAR P, ULONG Bytes)
{
    ULONG i;

    for (i = 0; i + A->GptEntrySize <= Bytes; i += A->GptEntrySize) {
        PUCHAR e = P + i;
        ULONGLONG attr;
        ULONG k;
        BOOLEAN used = FALSE;

        for (k = 0; k < 16; k++) {
            used |= e[k] != 0;
        }
        if (!used || NameIs(e, "esp") || NameIs(e, "win")) {
            continue;
        }
        attr = Le64(e + 48) | GPT_ATTR_HIDE;
        for (k = 0; k < 8; k++) {
            e[48 + k] = (UCHAR)(attr >> (8 * k));
        }
    }
}

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
        Say(A, "[ufs] gpt: READ CAPACITY failed, st %02X ocs %X is %08X\r\n", st, A->LastOcs, A->LastIs);
        return;
    }
    A->BlockSize = Be32(A->Bounce + 4);
    Say(A, "[ufs] gpt: capacity last lba %u, block %u\r\n", Be32(A->Bounce), A->BlockSize);
    if (A->BlockSize != 512 && A->BlockSize != 4096) {
        return;
    }
    if (!ReadBlocks(A, 1, 1)) {
        Say(A, "[ufs] gpt: read of lba 1 failed, ocs %X is %08X resp %02X %02X %02X %02X\r\n", A->LastOcs, A->LastIs,
            A->LastResponse[0], A->LastResponse[5], A->LastResponse[6], A->LastResponse[7]);
        return;
    }
    if (RtlCompareMemory(A->Bounce, "EFI PART", 8) != 8) {
        Say(A, "[ufs] gpt: lba 1 is not a GPT header: %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
            A->Bounce[0], A->Bounce[1], A->Bounce[2], A->Bounce[3], A->Bounce[4], A->Bounce[5], A->Bounce[6], A->Bounce[7]);
        return;
    }
    entLba = Le64(A->Bounce + 72);
    count = Le32(A->Bounce + 80);
    size = Le32(A->Bounce + 84);
    {
        ULONG hsize = Le32(A->Bounce + 12), hcrc = Le32(A->Bounce + 16), calc = 0;
        UCHAR h[512];

        if (hsize >= 92 && hsize <= sizeof(h)) {
            RtlCopyMemory(h, A->Bounce, hsize);
            h[16] = h[17] = h[18] = h[19] = 0;
            calc = Crc32(h, hsize, 0);
        }
        Say(A, "[ufs] gpt header: size %u crc %08X calc %08X, my lba %I64u alt %I64u, usable %I64u..%I64u, "
            "entries lba %I64u count %u size %u crc %08X\r\n", hsize, hcrc, calc, Le64(A->Bounce + 24), Le64(A->Bounce + 32),
            Le64(A->Bounce + 40), Le64(A->Bounce + 48), entLba, count, size, Le32(A->Bounce + 88));
        A->GptEntriesCrc = Le32(A->Bounce + 88);
        A->GptAltEntLba = Le64(A->Bounce + 32);     /* alt header; its entries just before it */
    }
    if (size != 128 || count == 0 || count > 256) {
        return;
    }
    blocks = (count * size + A->BlockSize - 1) / A->BlockSize;
    /* Entries one block at a time (one PRD each), then in one multi-PRD read: both must match the header. */
    {
        ULONG c1 = 0, c2, b, left = count * size;

        for (b = 0; b < blocks; b++) {
            ULONG n = left < A->BlockSize ? left : A->BlockSize;
            if (!ReadBlocks(A, entLba + b, 1)) {
                break;
            }
            c1 = Crc32(A->Bounce, n, c1);
            left -= n;
        }
        if (blocks * A->BlockSize > MAX_TRANSFER || !ReadBlocks(A, entLba, blocks)) {
            Say(A, "[ufs] gpt: multi-block read of the entries failed, ocs %X\r\n", A->LastOcs);
            return;
        }
        c2 = Crc32(A->Bounce, count * size, 0);
        Say(A, "[ufs] gpt entries crc: header %08X, block by block %08X, one %u-block read %08X%s\r\n",
            A->GptEntriesCrc, c1, blocks, c2, (c1 == A->GptEntriesCrc && c2 == A->GptEntriesCrc) ? " (all good)" : " MISMATCH");
        if (c2 == A->GptEntriesCrc && count * size == blocks * A->BlockSize) {
            /* Hide the Android partitions from Windows (see HideEntries), on a copy. */
            A->GptEntrySize = size;
            A->GptEntryCount = count;
            A->GptEntLba = entLba;
            A->GptEntBlocks = blocks;
            A->GptAltEntLba -= blocks;
            HideEntries(A, A->Bounce, count * size);
            A->GptHiddenCrc = Crc32(A->Bounce, count * size, 0);
            A->GptHide = TRUE;
            Say(A, "[ufs] gpt: Android partitions hidden from Windows, entries at %I64u and %I64u, crc %08X\r\n",
                A->GptEntLba, A->GptAltEntLba, A->GptHiddenCrc);
            if (!ReadBlocks(A, entLba, blocks)) {  /* the real entries back for the range scan */
                return;
            }
        }
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
    Say(A, "[ufs] gpt: block size %u, %u writable ranges", A->BlockSize, A->RangeCount);
    for (i = 0; i < A->RangeCount; i++) {
        Say(A, " [%I64u..%I64u]", A->Range[i].First, A->Range[i].Last);
    }
    Say(A, "\r\n");
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

/* ---------------------------------------------------------------- command log */

static PCSTR
ResultName(UCHAR R)
{
    switch (R) {
    case R_OK:       return "ok";
    case R_UNDERRUN: return "ok (short)";
    case R_CHECK:    return "CHECK CONDITION";
    case R_TIMEOUT:  return "TIMEOUT";
    case R_FAILED:   return "FAILED";
    case R_REFUSED:  return "refused (write protect / not allowed)";
    case R_LOCAL:    return "answered locally";
    default:         return "?";
    }
}

static VOID
Record(PADAPTER A, const UCHAR *Cdb, ULONG Len, UCHAR Result, UCHAR ScsiStatus, const UCHAR *Sense, ULONG Us)
{
    BOOLEAN error = Result == R_CHECK || Result == R_TIMEOUT || Result == R_FAILED || Result == R_REFUSED;
    ULONGLONG lba = 0;
    ULONG blocks = 0;
    UCHAR op = Cdb ? Cdb[0] : 0;

    A->Seq++;
    if (error) {
        A->Errors++;
    }
    if (A->Seq > LOG_ALL_FIRST && !error) {
        if ((A->Seq % 1000) == 0) {
            Say(A, "[ufs] %u commands, %u errors\r\n", A->Seq, A->Errors);
        }
        return;
    }
    switch (op) {
    case OP_READ10: case OP_WRITE10: case OP_VERIFY10:
        lba = Be32(Cdb + 2); blocks = Be16(Cdb + 7); break;
    case OP_READ16: case OP_WRITE16: case OP_VERIFY16:
        lba = Be64(Cdb + 2); blocks = Be32(Cdb + 10); break;
    default:
        break;
    }
    Say(A, "[ufs] #%u op %02X lba %I64u n %u len %u -> %s st %02X sense %X/%02X/%02X ocs %X res %u is %08X%s %u us\r\n",
        A->Seq, op, lba, blocks, Len, ResultName(Result), ScsiStatus,
        Sense ? (Sense[2] & 0xF) : 0, Sense ? Sense[12] : 0, Sense ? Sense[13] : 0,
        A->LastOcs, A->LastResidual, A->LastIs, A->Fatal ? " FATAL" : "", Us);
}

static ULONG
Micros(PADAPTER A, LARGE_INTEGER Start)
{
    LARGE_INTEGER now, freq;

    StorPortQueryPerformanceCounter(A, &freq, &now);
    return freq.QuadPart ? (ULONG)((now.QuadPart - Start.QuadPart) * 1000000 / freq.QuadPart) : 0;
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

/*
 * Samsung writes its GPT headers with HeaderSize 512. The UEFI spec allows anything from 92 to
 * the block size, but Windows accepts only 92 (sizeof its header struct): with 512 it rejects
 * the GPT and sees the protective MBR instead, so no partitions and no volumes. The first 92
 * bytes are the whole header (the rest is reserved, zero), so reads are answered with
 * HeaderSize 92 and the CRC recomputed over those 92 bytes. Nothing is written to the device:
 * the GPT is outside esp/win, so writes there are refused anyway.
 */
static VOID
FixGptHeaders(PADAPTER A, const UCHAR *Cdb, ULONG Len)
{
    ULONGLONG lba;
    ULONG i, blocks;

    if (A->BlockSize == 0 || Len < A->BlockSize) {
        return;
    }
    lba = Cdb[0] == OP_READ16 ? Be64(Cdb + 2) :
          Cdb[0] == OP_READ10 ? Be32(Cdb + 2) :
          (((ULONG)Cdb[1] & 0x1F) << 16) | ((ULONG)Cdb[2] << 8) | Cdb[3];
    blocks = Len / A->BlockSize;
    if (A->GptHide) {
        for (i = 0; i < blocks; i++) {
            ULONGLONG b = lba + i;

            if ((b >= A->GptEntLba && b < A->GptEntLba + A->GptEntBlocks) ||
                (b >= A->GptAltEntLba && b < A->GptAltEntLba + A->GptEntBlocks)) {
                HideEntries(A, A->Bounce + i * A->BlockSize, A->BlockSize);
            }
        }
    }
    for (i = 0; i < blocks; i++) {
        PUCHAR h = A->Bounce + i * A->BlockSize;
        ULONG size, crc;
        BOOLEAN hide;

        if (RtlCompareMemory(h, "EFI PART", 8) != 8 || Le64(h + 24) != lba + i) {
            continue;                       /* not a header, or not the one for this block */
        }
        size = Le32(h + 12);
        hide = A->GptHide && Le32(h + 88) == A->GptEntriesCrc && Le32(h + 80) == A->GptEntryCount &&
               Le32(h + 84) == A->GptEntrySize;
        if ((size == 92 && !hide) || size < 92 || size > A->BlockSize) {
            continue;
        }
        crc = Le32(h + 16);
        h[16] = h[17] = h[18] = h[19] = 0;
        if (Crc32(h, size, 0) != crc) {     /* corrupt as read: leave it for Windows to judge */
            h[16] = (UCHAR)crc; h[17] = (UCHAR)(crc >> 8); h[18] = (UCHAR)(crc >> 16); h[19] = (UCHAR)(crc >> 24);
            continue;
        }
        h[12] = 92; h[13] = h[14] = h[15] = 0;
        if (hide) {
            h[88] = (UCHAR)A->GptHiddenCrc; h[89] = (UCHAR)(A->GptHiddenCrc >> 8);
            h[90] = (UCHAR)(A->GptHiddenCrc >> 16); h[91] = (UCHAR)(A->GptHiddenCrc >> 24);
        }
        crc = Crc32(h, 92, 0);
        h[16] = (UCHAR)crc; h[17] = (UCHAR)(crc >> 8); h[18] = (UCHAR)(crc >> 16); h[19] = (UCHAR)(crc >> 24);
        if (A->GptFixes++ < 8) {
            Say(A, "[ufs] GPT header at lba %I64u: HeaderSize %u -> 92%s for Windows (crc %08X)\r\n", lba + i, size,
                hide ? ", Android partitions hidden" : "", crc);
        }
    }
}

static VOID
DoScsiInner(PADAPTER A, PSCSI_REQUEST_BLOCK Srb)
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
            RtlZeroMemory(A->Bounce, len);   /* a short answer reads as zeros, not stale data */
        } else if (Srb->SrbFlags & SRB_FLAGS_DATA_OUT) {
            dir = UTRD_DD_OUT;
            RtlCopyMemory(A->Bounce, Srb->DataBuffer, len);
        }
    }

    r = Exec(A, cdb, Srb->CdbLength, dir, len, &st, &res);
    switch (r) {
    case EXEC_OK:
        /* The residual this controller reports is not usable (INQUIRY len 64 reports 192 left,
         * READ CAPACITY(16) len 32 reports 32 left yet returns the data), so it is ignored:
         * the whole buffer is returned, zero-padded. */
        if (dir == UTRD_DD_IN) {
            if (cdb[0] == OP_READ6 || cdb[0] == OP_READ10 || cdb[0] == OP_READ16) {
                FixGptHeaders(A, cdb, len);
            }
            RtlCopyMemory(Srb->DataBuffer, A->Bounce, len);
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
        /* Controller-level failure (seen: OCS 7 on MODE SENSE len 192, READ CAPACITY(16) len 64).
         * For anything but a data transfer, answer ILLEGAL REQUEST so the class driver falls back
         * at once instead of retrying. */
        if (cdb[0] != OP_READ6 && cdb[0] != OP_READ10 && cdb[0] != OP_READ16 &&
            cdb[0] != OP_WRITE10 && cdb[0] != OP_WRITE16) {
            SetSense(Srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
            return;
        }
        Srb->SrbStatus = SRB_STATUS_ERROR;
        return;
    }
}

/* Every SCSI request, timed and logged. */
static VOID
DoScsi(PADAPTER A, PSCSI_REQUEST_BLOCK Srb)
{
    LARGE_INTEGER t0;
    UCHAR st, res, op = Srb->Cdb[0];
    const UCHAR *sense = NULL;

    StorPortQueryPerformanceCounter(A, NULL, &t0);
    DoScsiInner(A, Srb);
    st = SRB_STATUS(Srb->SrbStatus);
    if (st == SRB_STATUS_SELECTION_TIMEOUT) {
        return;                             /* not LUN 0: not interesting */
    }
    if ((Srb->SrbStatus & SRB_STATUS_AUTOSENSE_VALID) && Srb->SenseInfoBuffer != NULL && Srb->SenseInfoBufferLength >= 14) {
        sense = (const UCHAR *)Srb->SenseInfoBuffer;
    }
    if (st == SRB_STATUS_SUCCESS) {
        res = (op == OP_START_STOP || op == OP_VERIFY10 || op == OP_VERIFY16 || op == OP_UNMAP || op == OP_REPORT_LUNS) ? R_LOCAL : R_OK;
    } else if (st == SRB_STATUS_DATA_OVERRUN) {
        res = R_UNDERRUN;
    } else if (st == SRB_STATUS_TIMEOUT) {
        res = R_TIMEOUT;
    } else if (st == SRB_STATUS_ERROR && Srb->ScsiStatus == SCSISTAT_CHECK_CONDITION) {
        res = (sense != NULL && ((sense[2] & 0xF) == SCSI_SENSE_DATA_PROTECT ||
               ((sense[2] & 0xF) == SCSI_SENSE_ILLEGAL_REQUEST && sense[12] == SCSI_ADSENSE_ILLEGAL_COMMAND))) ? R_REFUSED : R_CHECK;
    } else {
        res = R_FAILED;
    }
    Record(A, Srb->Cdb, Srb->DataTransferLength, res, Srb->ScsiStatus, sense, Micros(A, t0));
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
    if (Srb->Function != SRB_FUNCTION_EXECUTE_SCSI) {
        Say(a, "[ufs] srb function %02X -> %02X\r\n", Srb->Function, Srb->SrbStatus);
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
    PADAPTER a = (PADAPTER)DeviceExtension;
    BOOLEAN ok = a->Ready ? TRUE : TakeOver(a);

    Say(a, "[ufs] HwInitialize -> %u (HCS %08X UTRLRSR %08X)\r\n", ok, Rd(a, REG_HCS), Rd(a, REG_UTRLRSR));
    return ok;
}

SCSI_ADAPTER_CONTROL_STATUS
HwAdapterControl(PVOID DeviceExtension, SCSI_ADAPTER_CONTROL_TYPE Type, PVOID Parameters)
{
    PSCSI_SUPPORTED_CONTROL_TYPE_LIST list;

    if (Type != ScsiQuerySupportedControlTypes) {
        Say((PADAPTER)DeviceExtension, "[ufs] adapter control %u\r\n", (ULONG)Type);
    }
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

    InboxStart(a);
    Note(a, "Step", 1);
    Note(a, "InterfaceType", (ULONG)ConfigInfo->AdapterInterfaceType);
    Note(a, "AccessRanges", ConfigInfo->NumberOfAccessRanges);
    r = (ConfigInfo->NumberOfAccessRanges >= 1 && ConfigInfo->AccessRanges != NULL) ? &(*ConfigInfo->AccessRanges)[0] : NULL;
    if (r != NULL) {
        Note(a, "RangeStart", r->RangeStart.LowPart);
        Note(a, "RangeLength", r->RangeLength);
        Note(a, "RangeInMemory", r->RangeInMemory);
    }
    if (r != NULL && r->RangeStart.QuadPart != 0 && r->RangeLength >= HCI_SIZE) {
        a->Hci = (PUCHAR)StorPortGetDeviceBase(a, ConfigInfo->AdapterInterfaceType, ConfigInfo->SystemIoBusNumber,
                                               r->RangeStart, HCI_SIZE, FALSE);
    }
    if (a->Hci == NULL) {
        STOR_PHYSICAL_ADDRESS hci;              /* no usable range from PnP: the known address */

        hci.QuadPart = 0x13100000;
        a->Hci = (PUCHAR)StorPortGetDeviceBase(a, ConfigInfo->AdapterInterfaceType, ConfigInfo->SystemIoBusNumber,
                                               hci, HCI_SIZE, FALSE);
        Note(a, "UsedFallbackAddress", 1);
    }
    if (a->Hci == NULL) {
        Note(a, "Step", 2);                     /* could not map the controller */
        return SP_RETURN_ERROR;
    }
    Note(a, "Step", 3);
    Note(a, "CAP", Rd(a, REG_CAP));
    Note(a, "HCE", Rd(a, REG_HCE));
    Note(a, "HCS", Rd(a, REG_HCS));
    Note(a, "UTRLDBR", Rd(a, REG_UTRLDBR));
    Note(a, "UTRLRSR", Rd(a, REG_UTRLRSR));
    Note(a, "IS", Rd(a, REG_IS));

    ConfigInfo->MaximumTransferLength = MAX_TRANSFER;
    ConfigInfo->NumberOfPhysicalBreaks = MAX_PRD;
    ConfigInfo->AlignmentMask = 3;
    ConfigInfo->ScatterGather = TRUE;
    ConfigInfo->Master = TRUE;
    ConfigInfo->CachesData = FALSE;
    ConfigInfo->NumberOfBuses = 1;
    /* Target 0 is the disk; the initiator (us) must have another id or target 0 is never scanned. */
    ConfigInfo->MaximumNumberOfTargets = 2;
    ConfigInfo->InitiatorBusId[0] = 1;
    ConfigInfo->MaximumNumberOfLogicalUnits = 1;
    ConfigInfo->MapBuffers = STOR_MAP_ALL_BUFFERS_INCLUDING_READ_WRITE;
    ConfigInfo->Dma64BitAddresses = 0;
    ConfigInfo->Dma32BitAddresses = TRUE;
    ConfigInfo->SynchronizationModel = StorSynchronizeHalfDuplex;
    ConfigInfo->MaxNumberOfIO = 1;
    ConfigInfo->InterruptSynchronizationMode = InterruptSupportNone;
    ConfigInfo->MaxIOsPerLun = 1;
    ConfigInfo->InitialLunQueueDepth = 1;

    a->Uncached = (PUCHAR)StorPortGetUncachedExtension(a, ConfigInfo, UNCACHED_SIZE);
    if (a->Uncached == NULL) {
        Note(a, "Step", 4);                     /* no uncached extension */
        return SP_RETURN_ERROR;
    }
    pa = StorPortGetPhysicalAddress(a, NULL, a->Uncached, &len);
    a->UncachedPhys = (ULONGLONG)pa.QuadPart;
    Note(a, "UncachedPhysLow", (ULONG)a->UncachedPhys);
    Note(a, "UncachedPhysHigh", (ULONG)(a->UncachedPhys >> 32));
    Note(a, "UncachedLen", len);
    if (len < UNCACHED_SIZE || (a->UncachedPhys & 0xFFF) != 0) {
        /* not one contiguous, page-aligned block: the offsets below would not hold */
        Note(a, "Step", 5);
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

    /* Take the controller over here, at PASSIVE_LEVEL, so the outcome can be noted. */
    Note(a, "TakeOver", TakeOver(a) ? 1 : 0);
    Note(a, "UTRLBAAfter", Rd(a, REG_UTRLBA));
    Note(a, "UTRLRSRAfter", Rd(a, REG_UTRLRSR));
    Note(a, "Step", 6);                         /* found */
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
#pragma warning(suppress: 4152)    /* storport declares HwFindAdapter as PVOID */
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
    /* Settings of the Galaxy S9+ Exynos9810Ufs miniport (boots Windows 11): without
     * NOT_REQUIRE_IO_PORT Storport wants an I/O port resource and start fails 0xC0000225. */
    init.FeatureSupport = STOR_FEATURE_SET_ADAPTER_INTERFACE_TYPE | STOR_FEATURE_ADAPTER_NOT_REQUIRE_IO_PORT;
    init.SrbTypeFlags = SRB_TYPE_FLAG_SCSI_REQUEST_BLOCK;
    init.AddressTypeFlags = ADDRESS_TYPE_FLAG_BTL8;

    /*
     * Register for every bus an ACPI device can come in on. Storport matches the PDO's legacy
     * bus type against these; older Storport (WinPE 19041) does not match InterfaceTypeUndefined
     * to an ACPI device and fails AddDevice with STATUS_NO_SUCH_DEVICE (code 31). The lowest
     * status wins, as for any multi-bus miniport.
     */
    {
        static const INTERFACE_TYPE types[] = { Internal, ACPIBus, PNPBus, InterfaceTypeUndefined };
        ULONG i, status, best = (ULONG)-1;

        for (i = 0; i < RTL_NUMBER_OF(types); i++) {
            init.AdapterInterfaceType = types[i];
            status = StorPortInitialize(DriverObject, RegistryPath, &init, NULL);
            if (status < best) {
                best = status;
            }
        }
        return best;
    }
}
