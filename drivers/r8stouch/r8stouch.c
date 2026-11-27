/*
 * r8stouch.sys - Galaxy S20 FE (r8s) touchscreen for Windows (KMDF HID minidriver).
 *
 * The panel's controller is a Zinitix ZT7650 at I2C address 0x20 on the pins
 * of HSI2C_15 (gpp5-6 SCL, gpp5-7 SDA). As in the r8sios XNU driver
 * (xnu/files/bsd/dev/redwood_zinitix.c, which works on this phone), the bus
 * is bit-banged on the GPIOs: open drain, a line is pulled low by making its
 * pin an output at 0 and let go by making it an input. No I2C controller, no
 * clocks. Power: the 3.0 V "tsp_ldo_en" regulator, enabled by gpm21-0.
 * Attention: gpa1-0 reads low while events are queued; the driver polls it.
 *
 * Binds to ACPI\RWOA0003 (DSDT \_SB.TSC0); _CRS: the PERIC1, ALIVE and CMGP
 * pinctrl blocks. Installed as a lower filter under mshidkmdf.sys, which
 * turns the HID IOCTLs below into a HID touch screen (10 contacts, parallel
 * mode) for Windows.
 *
 * What it found at start-up is written to the device's registry key
 * (Enum\ACPI\RWOA0003\...\Device Parameters): State, ChipCode, Revision,
 * Firmware, ResolutionX/Y, Attempts, Reads, Events, FailedReads.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <ntddk.h>
#include <wdf.h>
#include <hidport.h>

/* ---------------------------------------------------------------- hardware */

#define PINCTRL_PERIC1      0x10730000ULL   /* gpp5 at +0x00 */
#define PINCTRL_ALIVE       0x15850000ULL   /* gpa1 at +0x20 */
#define PINCTRL_CMGP        0x15C30000ULL   /* gpm21 at +0x2A0 */
#define PINCTRL_SIZE        0x1000

#define BANK_GPP5           0x000
#define BANK_GPA1           0x020
#define BANK_GPM21          0x2A0

#define GPIO_CON            0x00            /* 4 bits a pin */
#define GPIO_DAT            0x04
#define GPIO_PUD            0x08            /* 2 bits a pin */
#define FUNC_INPUT          0x0
#define FUNC_OUTPUT         0x1

#define PIN_SCL             6
#define PIN_SDA             7
#define PIN_ATTN            0
#define PIN_LDO             0

#define ZT_ADDR             0x20

#define ZT_CMD_SWRESET          0x0000
#define ZT_CMD_CLEAR_INT        0x0003
#define ZT_REG_TOUCH_MODE       0x0010
#define ZT_REG_CHIP_REVISION    0x0011
#define ZT_REG_FIRMWARE_VERSION 0x0012
#define ZT_REG_X_RESOLUTION     0x00C0
#define ZT_REG_Y_RESOLUTION     0x00C1
#define ZT_REG_POINT_STATUS     0x0200
#define ZT_REG_POINT_STATUS1    0x0201
#define ZT_VCMD_ENABLE          0x10F0
#define ZT_VCMD_PROGRAM_START   0x11F0
#define ZT_VCMD_NVM_INIT        0x12F0
#define ZT_VCMD_INTN_CLEAR      0x14F0
#define ZT_VCMD_CHIP_CODE       0x17F0
#define ZT_EVENT_SIZE           16
#define ZT_MAX_FINGERS          10
#define ZT_ACTION_PRESS         1
#define ZT_ACTION_MOVE          2
#define ZT_ACTION_RELEASE       3

#define HALF_US             2               /* bit-bang half period: ~125 kHz */
#define READ_PAUSE_US       50              /* register number, stop, pause, data */
#define POLL_IDLE_MS        8
#define POLL_ACTIVE_MS      2

/* ---------------------------------------------------------------- HID */

#define REPORTID_TOUCH      1
#define REPORTID_MAXCOUNT   2
#define REPORTID_MOUSE      3

/* Device Parameters\Mode: what the panel shows up as. */
#define MODE_MOUSE          1               /* one finger = an absolute mouse, touching = left button (default) */
#define MODE_TOUCH          2               /* a 10-finger HID touch screen */
#define MODE_BOTH           3
#define HID_MAX_X           32767

#define FINGER_COLLECTION \
    0x05, 0x0D,             /*   Usage Page (Digitizer) */             \
    0x09, 0x22,             /*   Usage (Finger) */                      \
    0xA1, 0x02,             /*   Collection (Logical) */                \
    0x09, 0x42,             /*     Usage (Tip Switch) */                \
    0x15, 0x00,             /*     Logical Minimum (0) */               \
    0x25, 0x01,             /*     Logical Maximum (1) */               \
    0x75, 0x01,             /*     Report Size (1) */                   \
    0x95, 0x01,             /*     Report Count (1) */                  \
    0x81, 0x02,             /*     Input (Data,Var,Abs) */              \
    0x95, 0x07,             /*     Report Count (7) */                  \
    0x81, 0x03,             /*     Input (Const) padding */             \
    0x09, 0x51,             /*     Usage (Contact Identifier) */        \
    0x75, 0x08,             /*     Report Size (8) */                   \
    0x95, 0x01,             /*     Report Count (1) */                  \
    0x26, 0xFF, 0x00,       /*     Logical Maximum (255) */             \
    0x81, 0x02,             /*     Input (Data,Var,Abs) */              \
    0x05, 0x01,             /*     Usage Page (Generic Desktop) */      \
    0x26, 0xFF, 0x7F,       /*     Logical Maximum (32767) */           \
    0x75, 0x10,             /*     Report Size (16) */                  \
    0x55, 0x0E,             /*     Unit Exponent (-2) */                \
    0x65, 0x11,             /*     Unit (cm) */                         \
    0x35, 0x00,             /*     Physical Minimum (0) */              \
    0x46, 0xA5, 0x02,       /*     Physical Maximum (677 = 6.77 cm) */  \
    0x09, 0x30,             /*     Usage (X) */                         \
    0x81, 0x02,             /*     Input (Data,Var,Abs) */              \
    0x46, 0xE1, 0x05,       /*     Physical Maximum (1505 = 15.05 cm) */\
    0x09, 0x31,             /*     Usage (Y) */                         \
    0x81, 0x02,             /*     Input (Data,Var,Abs) */              \
    0x45, 0x00,             /*     Physical Maximum (0) */              \
    0x55, 0x00,             /*     Unit Exponent (0) */                 \
    0x65, 0x00,             /*     Unit (none) */                       \
    0xC0                    /*   End Collection */

static const UCHAR g_ReportDescriptor[] = {
    0x05, 0x0D,             /* Usage Page (Digitizer) */
    0x09, 0x04,             /* Usage (Touch Screen) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, REPORTID_TOUCH,   /*   Report ID */
    FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION,
    FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION,
    0x05, 0x0D,             /*   Usage Page (Digitizer) */
    0x09, 0x54,             /*   Usage (Contact Count) */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x25, 0x7F,             /*   Logical Maximum (127) */
    0x75, 0x08,             /*   Report Size (8) */
    0x95, 0x01,             /*   Report Count (1) */
    0x81, 0x02,             /*   Input (Data,Var,Abs) */
    0x85, REPORTID_MAXCOUNT,/*   Report ID */
    0x09, 0x55,             /*   Usage (Contact Count Maximum) */
    0x25, ZT_MAX_FINGERS,   /*   Logical Maximum (10) */
    0xB1, 0x02,             /*   Feature (Data,Var,Abs) */
    0xC0,                   /* End Collection */

    /* Absolute mouse: what every Windows (WinPE too) moves the pointer with. */
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x02,             /* Usage (Mouse) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, REPORTID_MOUSE,   /*   Report ID */
    0x09, 0x01,             /*   Usage (Pointer) */
    0xA1, 0x00,             /*   Collection (Physical) */
    0x05, 0x09,             /*     Usage Page (Button) */
    0x19, 0x01,             /*     Usage Minimum (1) */
    0x29, 0x01,             /*     Usage Maximum (1) */
    0x15, 0x00,             /*     Logical Minimum (0) */
    0x25, 0x01,             /*     Logical Maximum (1) */
    0x75, 0x01,             /*     Report Size (1) */
    0x95, 0x01,             /*     Report Count (1) */
    0x81, 0x02,             /*     Input (Data,Var,Abs) */
    0x95, 0x07,             /*     Report Count (7) */
    0x81, 0x03,             /*     Input (Const) */
    0x05, 0x01,             /*     Usage Page (Generic Desktop) */
    0x09, 0x30,             /*     Usage (X) */
    0x09, 0x31,             /*     Usage (Y) */
    0x15, 0x00,             /*     Logical Minimum (0) */
    0x26, 0xFF, 0x7F,       /*     Logical Maximum (32767) */
    0x75, 0x10,             /*     Report Size (16) */
    0x95, 0x02,             /*     Report Count (2) */
    0x81, 0x02,             /*     Input (Data,Var,Abs) */
    0xC0,                   /*   End Collection */
    0xC0                    /* End Collection */
};

#include <pshpack1.h>
typedef struct _TOUCH_CONTACT {
    UCHAR  Tip;             /* bit 0 */
    UCHAR  ContactId;
    USHORT X;
    USHORT Y;
} TOUCH_CONTACT;

typedef struct _TOUCH_REPORT {
    UCHAR         ReportId;
    TOUCH_CONTACT Contact[ZT_MAX_FINGERS];
    UCHAR         ContactCount;
} TOUCH_REPORT;

typedef struct _MOUSE_REPORT {
    UCHAR  ReportId;
    UCHAR  Buttons;         /* bit 0 = left */
    USHORT X;
    USHORT Y;
} MOUSE_REPORT;

typedef struct _MAXCOUNT_REPORT {
    UCHAR ReportId;
    UCHAR MaxCount;
} MAXCOUNT_REPORT;
#include <poppack.h>

static const HID_DESCRIPTOR g_HidDescriptor = {
    sizeof(HID_DESCRIPTOR),     /* bLength */
    0x21,                       /* bDescriptorType: HID */
    0x0111,                     /* bcdHID */
    0x00,                       /* bCountry */
    0x01,                       /* bNumDescriptors */
    { 0x22, sizeof(g_ReportDescriptor) }   /* report descriptor */
};

/* ---------------------------------------------------------------- context */

typedef struct _FINGER {
    BOOLEAN Down;
    BOOLEAN Lifted;         /* report the lift once */
    USHORT  X, Y;
} FINGER;

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE   Device;
    WDFQUEUE    ReadQueue;          /* pended IOCTL_HID_READ_REPORT */
    volatile UCHAR *Peric1;
    volatile UCHAR *Alive;
    volatile UCHAR *Cmgp;
    volatile UCHAR *Gpp5;
    volatile UCHAR *Gpa1;
    volatile UCHAR *Gpm21;
    PKTHREAD    Thread;
    KEVENT      StopEvent;
    BOOLEAN     TimerResSet;
    ULONG       ResX, ResY;
    FINGER      Finger[ZT_MAX_FINGERS];
    ULONG       Reads, Events, FailedReads;
    volatile LONG Ioctl[10];        /* HID IOCTLs seen, by kind (see IoctlIndex) */
    volatile LONG ReadRequests, Sent, Dropped;
    ULONG       Mode;
    BOOLEAN     MouseDown;
    USHORT      MouseX, MouseY;
    LONG        FeatureStatus, FeatureInLen, FeatureOutLen, FeatureId;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY EvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT EvtD0Exit;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL EvtInternalIoctl;
KSTART_ROUTINE TouchThread;

/* ---------------------------------------------------------------- registry */

static VOID
SetValue(PDEVICE_CONTEXT Ctx, PCWSTR Name, ULONG Value)
{
    WDFKEY key;
    UNICODE_STRING name;

    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE,
                                             WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }
    RtlInitUnicodeString(&name, Name);
    (VOID)WdfRegistryAssignULong(key, &name, Value);
    WdfRegistryClose(key);
}

/* ---------------------------------------------------------------- GPIO */

static ULONG RdReg(volatile UCHAR *Bank, ULONG Off) { return READ_REGISTER_ULONG((volatile ULONG *)(Bank + Off)); }
static VOID  WrReg(volatile UCHAR *Bank, ULONG Off, ULONG V) { WRITE_REGISTER_ULONG((volatile ULONG *)(Bank + Off), V); }

static VOID
Update(volatile UCHAR *Bank, ULONG Off, ULONG Mask, ULONG Value)
{
    WrReg(Bank, Off, (RdReg(Bank, Off) & ~Mask) | (Value & Mask));
}

static VOID GpioFunc(volatile UCHAR *Bank, ULONG Pin, ULONG Func) { Update(Bank, GPIO_CON, 0xFu << (Pin * 4), Func << (Pin * 4)); }
static VOID GpioPull(volatile UCHAR *Bank, ULONG Pin, ULONG Pull) { Update(Bank, GPIO_PUD, 0x3u << (Pin * 2), Pull << (Pin * 2)); }
static ULONG GpioGet(volatile UCHAR *Bank, ULONG Pin) { return (RdReg(Bank, GPIO_DAT) >> Pin) & 1; }

static VOID
GpioSet(volatile UCHAR *Bank, ULONG Pin, ULONG Value)
{
    Update(Bank, GPIO_DAT, 1u << Pin, (Value ? 1u : 0u) << Pin);
    GpioFunc(Bank, Pin, FUNC_OUTPUT);
}

static VOID
SleepMs(ULONG Ms)
{
    LARGE_INTEGER t;

    t.QuadPart = -(LONGLONG)Ms * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

/* ---------------------------------------------------------------- bit-banged I2C */

#define BB_WAIT()       KeStallExecutionProcessor(HALF_US)
#define LOW(p)          GpioFunc(Ctx->Gpp5, (p), FUNC_OUTPUT)     /* DAT preset to 0 */
#define RELEASE(p)      GpioFunc(Ctx->Gpp5, (p), FUNC_INPUT)

static VOID
SclRelease(PDEVICE_CONTEXT Ctx)
{
    ULONG i;

    RELEASE(PIN_SCL);
    for (i = 0; i < 25000 && !GpioGet(Ctx->Gpp5, PIN_SCL); i++) {   /* clock stretching, up to ~25 ms */
        KeStallExecutionProcessor(1);
    }
}

static VOID
BbStart(PDEVICE_CONTEXT Ctx)
{
    RELEASE(PIN_SDA); BB_WAIT();
    SclRelease(Ctx);  BB_WAIT();
    LOW(PIN_SDA);     BB_WAIT();
    LOW(PIN_SCL);     BB_WAIT();
}

static VOID
BbStop(PDEVICE_CONTEXT Ctx)
{
    LOW(PIN_SDA);     BB_WAIT();
    SclRelease(Ctx);  BB_WAIT();
    RELEASE(PIN_SDA); BB_WAIT();
}

static BOOLEAN
BbWriteByte(PDEVICE_CONTEXT Ctx, UCHAR Byte)
{
    BOOLEAN ack;
    int i;

    for (i = 7; i >= 0; i--) {
        if ((Byte >> i) & 1) { RELEASE(PIN_SDA); } else { LOW(PIN_SDA); }
        BB_WAIT();
        SclRelease(Ctx);
        BB_WAIT();
        LOW(PIN_SCL);
    }
    RELEASE(PIN_SDA);
    BB_WAIT();
    SclRelease(Ctx);
    BB_WAIT();
    ack = (BOOLEAN)(GpioGet(Ctx->Gpp5, PIN_SDA) == 0);
    LOW(PIN_SCL);
    BB_WAIT();
    return ack;
}

static UCHAR
BbReadByte(PDEVICE_CONTEXT Ctx, BOOLEAN Ack)
{
    UCHAR value = 0;
    int i;

    RELEASE(PIN_SDA);
    for (i = 0; i < 8; i++) {
        BB_WAIT();
        SclRelease(Ctx);
        BB_WAIT();
        value = (UCHAR)((value << 1) | GpioGet(Ctx->Gpp5, PIN_SDA));
        LOW(PIN_SCL);
    }
    if (Ack) { LOW(PIN_SDA); } else { RELEASE(PIN_SDA); }
    BB_WAIT();
    SclRelease(Ctx);
    BB_WAIT();
    LOW(PIN_SCL);
    RELEASE(PIN_SDA);
    return value;
}

/* Write wlen bytes, or (wlen == 0) a plain read of rlen bytes. No combined transfers: the ZT7650
 * takes the register number, a stop, a pause, then a separate read (as in r8sios). */
static BOOLEAN
BbXfer(PDEVICE_CONTEXT Ctx, const UCHAR *W, ULONG WLen, UCHAR *R, ULONG RLen)
{
    BOOLEAN ok;
    ULONG i;

    BbStart(Ctx);
    if (WLen == 0) {
        ok = BbWriteByte(Ctx, (UCHAR)((ZT_ADDR << 1) | 1));
        for (i = 0; ok && i < RLen; i++) {
            R[i] = BbReadByte(Ctx, (BOOLEAN)(i + 1 < RLen));
        }
    } else {
        ok = BbWriteByte(Ctx, (UCHAR)(ZT_ADDR << 1));
        for (i = 0; ok && i < WLen; i++) {
            ok = BbWriteByte(Ctx, W[i]);
        }
    }
    BbStop(Ctx);
    return ok;
}

/* Take the pins, free a stuck bus. False: SDA/SCL held low (no power?). */
static BOOLEAN
BbSetup(PDEVICE_CONTEXT Ctx)
{
    ULONG i;

    RELEASE(PIN_SCL);
    RELEASE(PIN_SDA);
    Update(Ctx->Gpp5, GPIO_DAT, (1u << PIN_SCL) | (1u << PIN_SDA), 0);
    BB_WAIT();
    for (i = 0; i < 9 && !GpioGet(Ctx->Gpp5, PIN_SDA); i++) {
        LOW(PIN_SCL); BB_WAIT();
        SclRelease(Ctx); BB_WAIT();
    }
    LOW(PIN_SCL);
    BB_WAIT();
    BbStop(Ctx);
    return (BOOLEAN)(GpioGet(Ctx->Gpp5, PIN_SDA) && GpioGet(Ctx->Gpp5, PIN_SCL));
}

/* ---------------------------------------------------------------- Zinitix protocol */

static BOOLEAN
ZtCmd(PDEVICE_CONTEXT Ctx, USHORT Reg)
{
    UCHAR b[2] = { (UCHAR)Reg, (UCHAR)(Reg >> 8) };

    return BbXfer(Ctx, b, 2, NULL, 0);
}

static BOOLEAN
ZtWrite16(PDEVICE_CONTEXT Ctx, USHORT Reg, USHORT Value)
{
    UCHAR b[4] = { (UCHAR)Reg, (UCHAR)(Reg >> 8), (UCHAR)Value, (UCHAR)(Value >> 8) };

    return BbXfer(Ctx, b, 4, NULL, 0);
}

static BOOLEAN
ZtRead(PDEVICE_CONTEXT Ctx, USHORT Reg, UCHAR *Buf, ULONG Len, ULONG PauseUs)
{
    if (!ZtCmd(Ctx, Reg)) {
        return FALSE;
    }
    KeStallExecutionProcessor(PauseUs);
    return BbXfer(Ctx, NULL, 0, Buf, Len);
}

static USHORT
ZtRead16(PDEVICE_CONTEXT Ctx, USHORT Reg, ULONG PauseUs)
{
    UCHAR b[2] = { 0, 0 };

    (VOID)ZtRead(Ctx, Reg, b, 2, PauseUs);
    return (USHORT)(b[0] | (b[1] << 8));
}

static VOID
ZtPower(PDEVICE_CONTEXT Ctx, BOOLEAN Cycle)
{
    if (Cycle) {
        GpioSet(Ctx->Gpm21, PIN_LDO, 0);
        SleepMs(100);
    }
    GpioSet(Ctx->Gpm21, PIN_LDO, 1);
    SleepMs(Cycle ? 150 : 100);             /* CHIP_ON_DELAY */
}

static BOOLEAN
ZtInit(PDEVICE_CONTEXT Ctx)
{
    USHORT code, rev, fw, rx, ry;
    ULONG i;

    /* Vendor power-on sequence (Samsung's ZT7650 driver): enable vendor commands, start the firmware. */
    if (!ZtWrite16(Ctx, ZT_VCMD_ENABLE, 1)) {
        SetValue(Ctx, L"State", 3);         /* no ACK at 0x20 */
        return FALSE;
    }
    SleepMs(10);
    code = ZtRead16(Ctx, ZT_VCMD_CHIP_CODE, READ_PAUSE_US);
    if (code == ZT_VCMD_CHIP_CODE) {        /* echoed its own number: did not fetch; once more, slower */
        SleepMs(50);
        code = ZtRead16(Ctx, ZT_VCMD_CHIP_CODE, 2000);
    }
    SetValue(Ctx, L"ChipCode", code);
    SleepMs(10);
    if (!ZtCmd(Ctx, ZT_VCMD_INTN_CLEAR)) { return FALSE; }
    SleepMs(10);
    if (!ZtWrite16(Ctx, ZT_VCMD_NVM_INIT, 1)) { return FALSE; }
    SleepMs(2);
    if (!ZtWrite16(Ctx, ZT_VCMD_PROGRAM_START, 1)) { return FALSE; }
    SleepMs(150);                           /* FIRMWARE_ON_DELAY */

    if (!ZtCmd(Ctx, ZT_CMD_SWRESET)) {
        SetValue(Ctx, L"State", 4);
        return FALSE;
    }
    SleepMs(10);
    rev = ZtRead16(Ctx, ZT_REG_CHIP_REVISION, READ_PAUSE_US);
    fw = ZtRead16(Ctx, ZT_REG_FIRMWARE_VERSION, READ_PAUSE_US);
    rx = ZtRead16(Ctx, ZT_REG_X_RESOLUTION, READ_PAUSE_US);
    ry = ZtRead16(Ctx, ZT_REG_Y_RESOLUTION, READ_PAUSE_US);
    SetValue(Ctx, L"Revision", rev);
    SetValue(Ctx, L"Firmware", fw);
    if (rx >= 100 && rx <= 8192 && ry >= 100 && ry <= 8192) {
        Ctx->ResX = rx;
        Ctx->ResY = ry;
    }
    SetValue(Ctx, L"ResolutionX", Ctx->ResX);
    SetValue(Ctx, L"ResolutionY", Ctx->ResY);
    if (!ZtWrite16(Ctx, ZT_REG_TOUCH_MODE, 0)) {    /* TOUCH_POINT_MODE */
        SetValue(Ctx, L"State", 5);
        return FALSE;
    }
    for (i = 0; i < 10; i++) {
        (VOID)ZtCmd(Ctx, ZT_CMD_CLEAR_INT);
        KeStallExecutionProcessor(10);
    }
    return TRUE;
}

/* ---------------------------------------------------------------- reports */

static USHORT
Scale(ULONG V, ULONG Max)
{
    return (USHORT)(V >= Max ? HID_MAX_X : V * HID_MAX_X / Max);
}

/* One 16-byte ZT7650 event: byte 0 = event id (1:0), touch id (5:2), action (7:6);
 * x = b1 << 4 | b3 >> 4, y = b2 << 4 | (b3 & 0xf); byte 7 bits 3:0 = records still queued. */
static VOID
HandleEvent(PDEVICE_CONTEXT Ctx, const UCHAR *E)
{
    ULONG eid = E[0] & 3, slot = (E[0] >> 2) & 0xF, action = E[0] >> 6;
    ULONG x = ((ULONG)E[1] << 4) | (E[3] >> 4), y = ((ULONG)E[2] << 4) | (E[3] & 0xF);

    if (eid != 0 || slot >= ZT_MAX_FINGERS) {
        return;
    }
    if (action == ZT_ACTION_PRESS || action == ZT_ACTION_MOVE) {
        Ctx->Finger[slot].Down = TRUE;
        Ctx->Finger[slot].Lifted = FALSE;
        Ctx->Finger[slot].X = Scale(x, Ctx->ResX);
        Ctx->Finger[slot].Y = Scale(y, Ctx->ResY);
    } else if (action == ZT_ACTION_RELEASE && Ctx->Finger[slot].Down) {
        Ctx->Finger[slot].Down = FALSE;
        Ctx->Finger[slot].Lifted = TRUE;
    }
}

/* Complete one pended IOCTL_HID_READ_REPORT with Data, if HID class has one waiting. */
static VOID
Deliver(PDEVICE_CONTEXT Ctx, const VOID *Data, ULONG Size)
{
    WDFREQUEST req;
    WDFMEMORY mem;
    NTSTATUS status;

    if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Ctx->ReadQueue, &req))) {
        InterlockedIncrement(&Ctx->Dropped);    /* nobody listening: the next one is newer anyway */
        return;
    }
    InterlockedIncrement(&Ctx->Sent);
    status = WdfRequestRetrieveOutputMemory(req, &mem);
    if (NT_SUCCESS(status)) {
        status = WdfMemoryCopyFromBuffer(mem, 0, (PVOID)Data, Size);
    }
    WdfRequestCompleteWithInformation(req, status, NT_SUCCESS(status) ? Size : 0);
}

static VOID
SendTouch(PDEVICE_CONTEXT Ctx)
{
    TOUCH_REPORT rep;
    ULONG i, n = 0;

    RtlZeroMemory(&rep, sizeof(rep));
    rep.ReportId = REPORTID_TOUCH;
    for (i = 0; i < ZT_MAX_FINGERS; i++) {
        if (Ctx->Finger[i].Down || Ctx->Finger[i].Lifted) {
            rep.Contact[n].Tip = Ctx->Finger[i].Down ? 1 : 0;
            rep.Contact[n].ContactId = (UCHAR)i;
            rep.Contact[n].X = Ctx->Finger[i].X;
            rep.Contact[n].Y = Ctx->Finger[i].Y;
            n++;
        }
    }
    if (n != 0) {
        rep.ContactCount = (UCHAR)n;
        Deliver(Ctx, &rep, sizeof(rep));
    }
}

/* The lowest-numbered finger that is down drives the pointer; any finger down = left button. */
static VOID
SendMouse(PDEVICE_CONTEXT Ctx)
{
    MOUSE_REPORT rep;
    BOOLEAN down = FALSE;
    ULONG i;

    for (i = 0; i < ZT_MAX_FINGERS; i++) {
        if (Ctx->Finger[i].Down) {
            Ctx->MouseX = Ctx->Finger[i].X;
            Ctx->MouseY = Ctx->Finger[i].Y;
            down = TRUE;
            break;
        }
    }
    if (!down && !Ctx->MouseDown) {
        return;
    }
    Ctx->MouseDown = down;
    rep.ReportId = REPORTID_MOUSE;
    rep.Buttons = down ? 1 : 0;
    rep.X = Ctx->MouseX;
    rep.Y = Ctx->MouseY;
    Deliver(Ctx, &rep, sizeof(rep));
}

static VOID
SendReports(PDEVICE_CONTEXT Ctx)
{
    ULONG i;

    if (Ctx->Mode & MODE_TOUCH) {
        SendTouch(Ctx);
    }
    if (Ctx->Mode & MODE_MOUSE) {
        SendMouse(Ctx);
    }
    for (i = 0; i < ZT_MAX_FINGERS; i++) {
        Ctx->Finger[i].Lifted = FALSE;
    }
}

static BOOLEAN
ReadPoints(PDEVICE_CONTEXT Ctx)
{
    UCHAR first[ZT_EVENT_SIZE], rest[ZT_EVENT_SIZE * (ZT_MAX_FINGERS - 1)];
    ULONG left = 0, i;

    Ctx->Reads++;
    if (!ZtRead(Ctx, ZT_REG_POINT_STATUS, first, sizeof(first), READ_PAUSE_US)) {
        Ctx->FailedReads++;
        (VOID)ZtCmd(Ctx, ZT_CMD_CLEAR_INT);
        return FALSE;
    }
    if ((first[0] & 3) == 0) {
        left = first[7] & 0xF;
        if (left > ZT_MAX_FINGERS - 1) {
            left = ZT_MAX_FINGERS - 1;
        }
        if (left != 0 && !ZtRead(Ctx, ZT_REG_POINT_STATUS1, rest, left * ZT_EVENT_SIZE, READ_PAUSE_US)) {
            left = 0;
        }
    }
    HandleEvent(Ctx, first);
    for (i = 0; i < left; i++) {
        HandleEvent(Ctx, rest + i * ZT_EVENT_SIZE);
    }
    Ctx->Events += left + 1;
    (VOID)ZtCmd(Ctx, ZT_CMD_CLEAR_INT);
    SendReports(Ctx);
    return TRUE;
}

/* ---------------------------------------------------------------- thread */

static VOID
WriteCounters(PDEVICE_CONTEXT Ctx)
{
    static const PCWSTR names[10] = {
        L"IoctlDeviceDescriptor", L"IoctlReportDescriptor", L"IoctlAttributes", L"IoctlReadReport",
        L"IoctlGetFeature", L"IoctlActivate", L"IoctlDeactivate", L"IoctlIdle", L"IoctlGetString", L"IoctlOther"
    };
    ULONG i;

    SetValue(Ctx, L"Reads", Ctx->Reads);
    SetValue(Ctx, L"Events", Ctx->Events);
    SetValue(Ctx, L"FailedReads", Ctx->FailedReads);
    SetValue(Ctx, L"ReadRequests", (ULONG)Ctx->ReadRequests);
    SetValue(Ctx, L"ReportsSent", (ULONG)Ctx->Sent);
    SetValue(Ctx, L"ReportsDropped", (ULONG)Ctx->Dropped);
    for (i = 0; i < 10; i++) {
        SetValue(Ctx, names[i], (ULONG)Ctx->Ioctl[i]);
    }
    SetValue(Ctx, L"ModeInUse", Ctx->Mode);
    SetValue(Ctx, L"FeatureStatus", (ULONG)Ctx->FeatureStatus);
    SetValue(Ctx, L"FeatureInLen", (ULONG)Ctx->FeatureInLen);
    SetValue(Ctx, L"FeatureOutLen", (ULONG)Ctx->FeatureOutLen);
    SetValue(Ctx, L"FeatureId", (ULONG)Ctx->FeatureId);
}

VOID
TouchThread(PVOID Context)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    LARGE_INTEGER zero, idle, active;
    ULONG attempt, ticks = 0;
    BOOLEAN ready = FALSE;
    LONGLONG activeUntil = 0;

    zero.QuadPart = 0;
    idle.QuadPart = -(LONGLONG)POLL_IDLE_MS * 10000;
    active.QuadPart = -(LONGLONG)POLL_ACTIVE_MS * 10000;

    GpioFunc(ctx->Gpa1, PIN_ATTN, FUNC_INPUT);
    GpioPull(ctx->Gpa1, PIN_ATTN, 0);

    for (attempt = 1; attempt <= 5 && !ready; attempt++) {
        SetValue(ctx, L"Attempts", attempt);
        ZtPower(ctx, (BOOLEAN)(attempt > 1));
        if (!BbSetup(ctx)) {
            SetValue(ctx, L"State", 2);      /* bus held low */
        } else if (ZtInit(ctx)) {
            ready = TRUE;
        }
        if (!ready && KeWaitForSingleObject(&ctx->StopEvent, Executive, KernelMode, FALSE, &idle) == STATUS_SUCCESS) {
            PsTerminateSystemThread(STATUS_SUCCESS);
        }
    }
    if (!ready) {
        PsTerminateSystemThread(STATUS_SUCCESS);      /* State says why */
    }
    SetValue(ctx, L"State", 1);              /* running */
    WriteCounters(ctx);

    for (;;) {
        LARGE_INTEGER now;

        if (KeWaitForSingleObject(&ctx->StopEvent, Executive, KernelMode, FALSE, &zero) == STATUS_SUCCESS) {
            break;
        }
        KeQuerySystemTimePrecise(&now);
        if (GpioGet(ctx->Gpa1, PIN_ATTN) == 0 && ReadPoints(ctx)) {
            activeUntil = now.QuadPart + 100 * 10000;       /* fast polling for 100 ms */
        }
        if (++ticks >= 1000) {               /* every ~10 s: counters into the registry */
            ticks = 0;
            WriteCounters(ctx);
        }
        if (KeWaitForSingleObject(&ctx->StopEvent, Executive, KernelMode, FALSE,
                                  now.QuadPart < activeUntil ? &active : &idle) == STATUS_SUCCESS) {
            break;
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---------------------------------------------------------------- PnP / power */

static volatile UCHAR *
MapBlock(PHYSICAL_ADDRESS Pa)
{
    return (volatile UCHAR *)MmMapIoSpaceEx(Pa, PINCTRL_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
}

NTSTATUS
EvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT ctx = GetContext(Device);
    PHYSICAL_ADDRESS pa;
    ULONG i, n, mem = 0;

    UNREFERENCED_PARAMETER(Raw);

    /* _CRS order: PERIC1, ALIVE, CMGP. Fall back to the known addresses. */
    n = WdfCmResourceListGetCount(Translated);
    for (i = 0; i < n; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(Translated, i);

        if (d == NULL || d->Type != CmResourceTypeMemory) {
            continue;
        }
        if (mem == 0) { ctx->Peric1 = MapBlock(d->u.Memory.Start); }
        if (mem == 1) { ctx->Alive = MapBlock(d->u.Memory.Start); }
        if (mem == 2) { ctx->Cmgp = MapBlock(d->u.Memory.Start); }
        mem++;
    }
    if (ctx->Peric1 == NULL) { pa.QuadPart = PINCTRL_PERIC1; ctx->Peric1 = MapBlock(pa); }
    if (ctx->Alive == NULL)  { pa.QuadPart = PINCTRL_ALIVE;  ctx->Alive = MapBlock(pa); }
    if (ctx->Cmgp == NULL)   { pa.QuadPart = PINCTRL_CMGP;   ctx->Cmgp = MapBlock(pa); }
    if (ctx->Peric1 == NULL || ctx->Alive == NULL || ctx->Cmgp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ctx->Gpp5 = ctx->Peric1 + BANK_GPP5;
    ctx->Gpa1 = ctx->Alive + BANK_GPA1;
    ctx->Gpm21 = ctx->Cmgp + BANK_GPM21;
    ctx->ResX = 1080;
    ctx->ResY = 2400;
    ctx->Mode = MODE_MOUSE;
    {
        WDFKEY key;
        UNICODE_STRING name;
        ULONG mode;

        if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
            RtlInitUnicodeString(&name, L"Mode");
            if (NT_SUCCESS(WdfRegistryQueryULong(key, &name, &mode)) && mode >= MODE_MOUSE && mode <= MODE_BOTH) {
                ctx->Mode = mode;
            }
            WdfRegistryClose(key);
        }
    }
    SetValue(ctx, L"State", 0);              /* hardware mapped, not started */
    return STATUS_SUCCESS;
}

NTSTATUS
EvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT ctx = GetContext(Device);

    UNREFERENCED_PARAMETER(Translated);
    if (ctx->Peric1) { MmUnmapIoSpace((PVOID)ctx->Peric1, PINCTRL_SIZE); ctx->Peric1 = NULL; }
    if (ctx->Alive)  { MmUnmapIoSpace((PVOID)ctx->Alive, PINCTRL_SIZE);  ctx->Alive = NULL; }
    if (ctx->Cmgp)   { MmUnmapIoSpace((PVOID)ctx->Cmgp, PINCTRL_SIZE);   ctx->Cmgp = NULL; }
    return STATUS_SUCCESS;
}

NTSTATUS
EvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous)
{
    PDEVICE_CONTEXT ctx = GetContext(Device);
    HANDLE h;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Previous);
    /* 1 ms timer ticks, so the 2/8 ms polls are not rounded up to 15.6 ms. */
    ExSetTimerResolution(10000, TRUE);
    ctx->TimerResSet = TRUE;
    KeClearEvent(&ctx->StopEvent);
    status = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, TouchThread, ctx);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&ctx->Thread, NULL);
    ZwClose(h);
    if (NT_SUCCESS(status)) {
        KeSetPriorityThread(ctx->Thread, LOW_REALTIME_PRIORITY);
    }
    return status;
}

NTSTATUS
EvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    PDEVICE_CONTEXT ctx = GetContext(Device);

    UNREFERENCED_PARAMETER(Target);
    KeSetEvent(&ctx->StopEvent, IO_NO_INCREMENT, FALSE);
    if (ctx->Thread != NULL) {
        KeWaitForSingleObject(ctx->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(ctx->Thread);
        ctx->Thread = NULL;
    }
    if (ctx->TimerResSet) {
        ExSetTimerResolution(0, FALSE);
        ctx->TimerResSet = FALSE;
    }
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- HID IOCTLs */

static ULONG
IoctlIndex(ULONG Code)
{
    switch (Code) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:           return 0;
    case IOCTL_HID_GET_REPORT_DESCRIPTOR:           return 1;
    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:           return 2;
    case IOCTL_HID_READ_REPORT:                     return 3;
    case IOCTL_HID_GET_FEATURE:                     return 4;
    case IOCTL_HID_ACTIVATE_DEVICE:                 return 5;
    case IOCTL_HID_DEACTIVATE_DEVICE:               return 6;
    case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:  return 7;
    case IOCTL_HID_GET_STRING:                      return 8;
    default:                                        return 9;
    }
}

static NTSTATUS
CopyOut(WDFREQUEST Request, const VOID *Data, ULONG Size)
{
    WDFMEMORY mem;
    NTSTATUS status = WdfRequestRetrieveOutputMemory(Request, &mem);

    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = WdfMemoryCopyFromBuffer(mem, 0, (PVOID)Data, Size);
    if (NT_SUCCESS(status)) {
        WdfRequestSetInformation(Request, Size);
    }
    return status;
}

VOID
EvtInternalIoctl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLen, size_t InLen, ULONG Code)
{
    PDEVICE_CONTEXT ctx = GetContext(WdfIoQueueGetDevice(Queue));
    NTSTATUS status = STATUS_NOT_SUPPORTED;

    InterlockedIncrement(&ctx->Ioctl[IoctlIndex(Code)]);
    switch (Code) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        status = CopyOut(Request, &g_HidDescriptor, sizeof(g_HidDescriptor));
        break;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        status = CopyOut(Request, g_ReportDescriptor, sizeof(g_ReportDescriptor));
        break;

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES: {
        HID_DEVICE_ATTRIBUTES attr;

        RtlZeroMemory(&attr, sizeof(attr));
        attr.Size = sizeof(attr);
        attr.VendorID = 0x04E8;         /* Samsung */
        attr.ProductID = 0x7650;        /* ZT7650 */
        attr.VersionNumber = 0x0100;
        status = CopyOut(Request, &attr, sizeof(attr));
        break;
    }

    case IOCTL_HID_READ_REPORT:
        InterlockedIncrement(&ctx->ReadRequests);
        status = WdfRequestForwardToIoQueue(Request, ctx->ReadQueue);
        if (NT_SUCCESS(status)) {
            return;                     /* completed by the thread with the next touch */
        }
        break;

    case IOCTL_HID_GET_FEATURE: {
        WDFMEMORY inMem;
        size_t inSize = 0;
        UCHAR id = REPORTID_MAXCOUNT;   /* our only feature report, if the id can't be read */
        MAXCOUNT_REPORT rep;

        /* mshidkmdf: input buffer = the report id, output buffer = the report */
        if (NT_SUCCESS(WdfRequestRetrieveInputMemory(Request, &inMem))) {
            PUCHAR p = (PUCHAR)WdfMemoryGetBuffer(inMem, &inSize);
            if (p != NULL && inSize >= 1) {
                id = p[0];
            }
        }
        ctx->FeatureInLen = (LONG)InLen;
        ctx->FeatureOutLen = (LONG)OutLen;
        ctx->FeatureId = id;
        if (id != REPORTID_MAXCOUNT) {
            status = STATUS_INVALID_PARAMETER;
        } else {
            rep.ReportId = REPORTID_MAXCOUNT;
            rep.MaxCount = ZT_MAX_FINGERS;
            status = CopyOut(Request, &rep, sizeof(rep));
        }
        ctx->FeatureStatus = status;
        break;
    }

    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
    case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
        status = STATUS_SUCCESS;
        break;

    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }
    WdfRequestComplete(Request, status);
}

NTSTATUS
EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_IO_QUEUE_CONFIG qcfg;
    WDFDEVICE device;
    PDEVICE_CONTEXT ctx;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    WdfFdoInitSetFilter(Init);      /* lower filter under mshidkmdf.sys */

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
    pnp.EvtDeviceD0Entry = EvtD0Entry;
    pnp.EvtDeviceD0Exit = EvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&Init, &attrs, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ctx = GetContext(device);
    ctx->Device = device;
    KeInitializeEvent(&ctx->StopEvent, NotificationEvent, FALSE);

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchParallel);
    qcfg.EvtIoInternalDeviceControl = EvtInternalIoctl;
    status = WdfIoQueueCreate(device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT(&qcfg, WdfIoQueueDispatchManual);
    qcfg.PowerManaged = WdfFalse;
    return WdfIoQueueCreate(device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, &ctx->ReadQueue);
}

NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;

    WDF_DRIVER_CONFIG_INIT(&cfg, EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
