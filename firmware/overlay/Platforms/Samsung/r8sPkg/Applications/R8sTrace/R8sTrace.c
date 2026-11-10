/** @file
  r8s-woa firmware call tracer.

  Wraps the Boot Services and Runtime Services tables, then loads and starts
  \efi\boot\winpe.efi (Windows Boot Manager). Every firmware call Boot
  Manager makes is printed through DEBUG(), which DEBUG builds of Silicium
  draw straight onto the framebuffer, so when Boot Manager hangs, the last
  lines show what it last asked the firmware for. Repeated calls collapse
  (printed at 1, 2, 4, 8 ... repeats). Original tables are restored at
  ExitBootServices so the OS never calls into this (freed) image.

  Every line is also appended to \r8s-trace.txt on the same partition and
  flushed straight away (when the TPL allows file I/O), so the log survives
  a hang and can be pulled from TWRP. Works with RELEASE firmware too (the
  on-screen copy needs a DEBUG build).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/MemoryAttribute.h>
#include <Guid/Acpi.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Guid/FileInfo.h>

STATIC EFI_BOOT_SERVICES     mBs;     // copy of the original tables
STATIC EFI_RUNTIME_SERVICES  mRt;
STATIC CONST CHAR8           *mLast;
STATIC UINTN                 mRepeat;
STATIC BOOLEAN               mActive;

STATIC EFI_FILE_PROTOCOL     *mLog;
STATIC BOOLEAN               mInLog;
STATIC CHAR8                 mLine[256];
STATIC CHAR8                 mPending[8192];
STATIC UINTN                 mPendingLen;

//
// Write one line to the screen (DEBUG builds) and to \r8s-trace.txt.
// File I/O runs only at TPL <= TPL_CALLBACK; otherwise the line waits in
// mPending until the next call that can write. mInLog stops the FAT driver's
// own firmware calls from being traced while we write.
//
STATIC
VOID
Emit (
  IN CONST CHAR8  *Line
  )
{
  EFI_TPL  Tpl;
  UINTN    Len;
  UINTN    Size;

  DEBUG ((DEBUG_ERROR, "%a", Line));
  if (mLog == NULL) {
    return;
  }

  Len = AsciiStrLen (Line);
  if (mPendingLen + Len < sizeof (mPending)) {
    CopyMem (mPending + mPendingLen, Line, Len);
    mPendingLen += Len;
  }

  Tpl = mBs.RaiseTPL (TPL_HIGH_LEVEL);
  mBs.RestoreTPL (Tpl);
  if (Tpl >= TPL_CALLBACK) {      // FAT and disk drivers lock at TPL_CALLBACK
    return;
  }

  mInLog = TRUE;
  Size   = mPendingLen;
  mLog->Write (mLog, &Size, mPending);
  mLog->Flush (mLog);
  mPendingLen = 0;
  mInLog      = FALSE;
}

//
// Only calls made by Boot Manager are traced: the return address must lie in
// its image or in EfiLoaderCode pages it allocated (its context-switch code
// is copied there). Firmware-internal calls (FAT reading winpe.efi, disk
// I/O) are skipped, so the log file is never written in the middle of one.
//
STATIC UINT64  mRangeBase[16];
STATIC UINT64  mRangeEnd[16];
STATIC UINTN   mRangeCount;

STATIC
VOID
AddRange (
  IN UINT64  Base,
  IN UINT64  Size
  )
{
  if (mRangeCount < ARRAY_SIZE (mRangeBase)) {
    mRangeBase[mRangeCount] = Base;
    mRangeEnd[mRangeCount]  = Base + Size;
    mRangeCount++;
  }
}

STATIC
BOOLEAN
FromBootmgr (
  IN UINTN  ReturnAddress
  )
{
  UINTN  Index;

  for (Index = 0; Index < mRangeCount; Index++) {
    if ((ReturnAddress >= mRangeBase[Index]) && (ReturnAddress < mRangeEnd[Index])) {
      return TRUE;
    }
  }

  return FALSE;
}

#define CALLER  ((UINTN)__builtin_return_address (0))

#define EMIT(...)                                                             \
  do {                                                                        \
    if (mInLog) break;                                                        \
    AsciiSPrint (mLine, sizeof (mLine), __VA_ARGS__);                         \
    Emit (mLine);                                                             \
  } while (0)

#define T(Name, Fmt, ...)                                                     \
  do {                                                                        \
    if (!mActive || mInLog || !FromBootmgr (CALLER)) break;                   \
    if (mLast == (Name)) {                                                    \
      mRepeat++;                                                              \
      if ((mRepeat & (mRepeat - 1)) != 0) break;                              \
      EMIT ("T> %a x%d " Fmt "\n", (Name), (UINT32)mRepeat, ##__VA_ARGS__);   \
    } else {                                                                  \
      mLast = (Name); mRepeat = 1;                                            \
      EMIT ("T> %a " Fmt "\n", (Name), ##__VA_ARGS__);                        \
    }                                                                         \
  } while (0)

#define R(Name, Status)                                                       \
  do { if (mActive && !mInLog && mRepeat == 1 && FromBootmgr (CALLER)) EMIT ("   %a = %r\n", (Name), (Status)); } while (0)

STATIC
VOID
LogOpen (
  IN EFI_HANDLE  Device
  )
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;

  Status = gBS->HandleProtocol (Device, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status) || EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
    Print (L"Trace file: no file system (%r)\n", Status);
    return;
  }

  // Truncate: open, delete, create again.
  if (!EFI_ERROR (Root->Open (Root, &File, L"\\r8s-trace.txt", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
    File->Delete (File);
  }

  Status = Root->Open (Root, &mLog, L"\\r8s-trace.txt", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  Root->Close (Root);
  if (EFI_ERROR (Status)) {
    mLog = NULL;
  }

  Print (L"Trace file \\r8s-trace.txt: %r\n", Status);
}

STATIC
VOID
LogClose (
  VOID
  )
{
  EFI_TPL  Tpl;

  if (mLog == NULL) {
    return;
  }

  Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  gBS->RestoreTPL (Tpl);
  if (Tpl <= TPL_CALLBACK) {
    mInLog = TRUE;
    mLog->Flush (mLog);
    mLog->Close (mLog);
    mInLog = FALSE;
  }

  mLog = NULL;
}

//
// Heartbeat: a 1-second timer event that draws straight into the
// framebuffer. Top-right square flips green/red each second; a bar along
// the bottom grows one step per second. If it keeps going while Boot
// Manager "hangs", the firmware's timer interrupt still runs (Boot Manager
// is busy or waiting); if it stops, interrupts are off or the CPU is wedged.
//
STATIC EFI_EVENT  mBeat;
STATIC UINT32     *mFb;
STATIC UINT32     mFbW;
STATIC UINT32     mFbH;
STATIC UINT32     mFbStride;
STATIC UINTN      mTicks;

STATIC
VOID
FillRect (
  UINT32  X,
  UINT32  Y,
  UINT32  W,
  UINT32  H,
  UINT32  Color
  )
{
  UINT32  Row;
  UINT32  Col;

  for (Row = Y; Row < Y + H && Row < mFbH; Row++) {
    for (Col = X; Col < X + W && Col < mFbW; Col++) {
      mFb[Row * mFbStride + Col] = Color;
    }
  }
}

STATIC
VOID
EFIAPI
Heartbeat (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT32  Step;
  UINT32  Lap;

  mTicks++;
  FillRect (mFbW - 200, 60, 160, 160, (mTicks & 1) ? 0x0000FF00 : 0x00FF0000);

  Step = 24;
  Lap  = (UINT32)((mTicks * Step) / mFbW);
  FillRect ((UINT32)((mTicks * Step) % mFbW), mFbH - 90, Step - 4, 60, (Lap & 1) ? 0x00FFFF00 : 0x0000FFFF);
}

STATIC
VOID
HeartbeatStart (
  VOID
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;

  if (EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop))) {
    return;
  }

  mFb       = (UINT32 *)(UINTN)Gop->Mode->FrameBufferBase;
  mFbW      = Gop->Mode->Info->HorizontalResolution;
  mFbH      = Gop->Mode->Info->VerticalResolution;
  mFbStride = Gop->Mode->Info->PixelsPerScanLine;

  if (!EFI_ERROR (gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_NOTIFY, Heartbeat, NULL, &mBeat))) {
    gBS->SetTimer (mBeat, TimerPeriodic, 10000000);   // 1 s
  }
}

STATIC
VOID
HeartbeatStop (
  VOID
  )
{
  if (mBeat != NULL) {
    mBs.CloseEvent (mBeat);
    mBeat = NULL;
  }
}

//
// Breadcrumbs inside Boot Manager. Boot Manager hangs between two firmware
// calls, inside BlpMmInitialize, where the tracer cannot see. For the exact
// bootmgfw build in our WinPE (22621.1, PDB GUID EB6298F7-0E4F-53A4-F859-
// F553FA1960AA), patch the first instruction of a few functions with a branch
// to a trampoline that paints a 100x100 square into the framebuffer, runs the
// displaced instruction and branches back. Squares sit along the bottom of
// the screen in call order; the last one painted shows how far it got.
// Every patched instruction is checked first; on any mismatch nothing is
// patched.
//
typedef struct {
  CONST CHAR8  *Name;
  UINT32       Rva;
  UINT32       Expect;      // original first instruction
  UINT32       Color;
} CRUMB;

STATIC CONST CRUMB  mCrumbs[] = {
  { "MmPaInitialize (before the 0x80000000 request)", 0x0D6D78, 0xA9BC53F3, 0x0000FF00 },
  { "BlMmRemoveBadMemory",                           0x0D6A60, 0xA9BE53F3, 0x00FFFFFF },
  { "MmTrInitialize",                                0x0D8DE8, 0xA9BC53F3, 0x00FFFFFF },
  { "MmHapHeapAllocatorExtend",                      0x0D5DF0, 0xA9BE53F3, 0x00FFFFFF },
  { "MmMdInitialize",                                0x0DAA70, 0xA9BE53F3, 0x00FFFFFF },
  { "BlUtlGetAcpiTable",                             0x0B7A98, 0xA9BC53F3, 0x00FFFFFF },
};

// stp x14,x15,[sp,#-32]!; stp x16,x17,[sp,#16]; ldr x16,addr; ldr x17,color;
// ldr x14,stride; mov x15,#100; 25 x stp x17,x17,[x16,#n*16]; add x16,x16,x14;
// subs x15,x15,#1; b.ne row; dsb sy; ldp x16,x17,[sp,#16]; ldp x14,x15,[sp],#32;
// <displaced>; b back; .quad addr; .quad color; .quad stride
STATIC CONST UINT32  mTramp[45] = {
  0xA9BE3FEE, 0xA90147F0, 0x580004B0, 0x580004D1, 0x580004EE, 0xD2800C8F, 0xA9004611, 0xA9014611,
  0xA9024611, 0xA9034611, 0xA9044611, 0xA9054611, 0xA9064611, 0xA9074611, 0xA9084611, 0xA9094611,
  0xA90A4611, 0xA90B4611, 0xA90C4611, 0xA90D4611, 0xA90E4611, 0xA90F4611, 0xA9104611, 0xA9114611,
  0xA9124611, 0xA9134611, 0xA9144611, 0xA9154611, 0xA9164611, 0xA9174611, 0xA9184611, 0x8B0E0210,
  0xF10005EF, 0x54FFFCA1, 0xD5033F9F, 0xA94147F0, 0xA8C23FEE, 0xD503201F, 0xD503201F, 0, 0, 0, 0, 0, 0
};
#define TRAMP_DISPLACED  37
#define TRAMP_BACK       38
#define TRAMP_ADDR       39
#define TRAMP_COLOR      41
#define TRAMP_STRIDE     43
#define TRAMP_SLOT       64            // words per trampoline (256 bytes)

STATIC
UINT32
EncodeB (
  UINT64  From,
  UINT64  To
  )
{
  INT64  Off = ((INT64)To - (INT64)From) / 4;

  return 0x14000000 | ((UINT32)Off & 0x03FFFFFF);
}

STATIC
VOID
InstallCrumbs (
  IN UINT64  ImageBase,
  IN UINT64  ImageSize
  )
{
  EFI_STATUS                     Status;
  EFI_MEMORY_ATTRIBUTE_PROTOCOL  *MemAttr;
  EFI_PHYSICAL_ADDRESS           Tramp;
  UINT32                         *Code;
  UINT32                         *T;
  UINT64                         Square;
  UINTN                          Index;

  if (mFb == NULL) {
    EMIT ("T> crumbs: no framebuffer\n");
    return;
  }

  if (ImageSize != 0x1F8000) {
    EMIT ("T> crumbs: Boot Manager size 0x%lx is not the 22621.1 build, skipped\n", ImageSize);
    return;
  }

  for (Index = 0; Index < ARRAY_SIZE (mCrumbs); Index++) {
    if (*(UINT32 *)(UINTN)(ImageBase + mCrumbs[Index].Rva) != mCrumbs[Index].Expect) {
      EMIT ("T> crumbs: %a has 0x%x, expected 0x%x; skipped\n", mCrumbs[Index].Name,
            *(UINT32 *)(UINTN)(ImageBase + mCrumbs[Index].Rva), mCrumbs[Index].Expect);
      return;
    }
  }

  // Trampolines must be within +-128 MB of the patched code (B range).
  Tramp  = ImageBase + 0x7000000;
  Status = gBS->AllocatePages (AllocateMaxAddress, EfiLoaderCode, 1, &Tramp);
  if (EFI_ERROR (Status) || (Tramp + 0x8000000 < ImageBase)) {
    EMIT ("T> crumbs: no trampoline page (%r)\n", Status);
    return;
  }

  // Image code is mapped read-only by the firmware's memory protection.
  if (!EFI_ERROR (gBS->LocateProtocol (&gEfiMemoryAttributeProtocolGuid, NULL, (VOID **)&MemAttr))) {
    Status = MemAttr->ClearMemoryAttributes (MemAttr, ImageBase, ALIGN_VALUE (ImageSize, EFI_PAGE_SIZE), EFI_MEMORY_RO);
    EMIT ("T> crumbs: image RO cleared: %r\n", Status);
    MemAttr->ClearMemoryAttributes (MemAttr, Tramp, EFI_PAGE_SIZE, EFI_MEMORY_RO | EFI_MEMORY_XP);
  } else {
    EMIT ("T> crumbs: no memory attribute protocol, trying anyway\n");
  }

  for (Index = 0; Index < ARRAY_SIZE (mCrumbs); Index++) {
    Code   = (UINT32 *)(UINTN)(ImageBase + mCrumbs[Index].Rva);
    T      = (UINT32 *)(UINTN)Tramp + Index * TRAMP_SLOT;
    Square = (UINT64)(UINTN)mFb + ((UINT64)(mFbH - 400) * mFbStride + 40 + Index * 170) * 4;

    CopyMem (T, mTramp, sizeof (mTramp));
    T[TRAMP_DISPLACED] = mCrumbs[Index].Expect;
    T[TRAMP_BACK]      = EncodeB ((UINT64)(UINTN)&T[TRAMP_BACK], (UINT64)(UINTN)(Code + 1));
    *(UINT64 *)&T[TRAMP_ADDR]   = Square;
    *(UINT64 *)&T[TRAMP_COLOR]  = ((UINT64)mCrumbs[Index].Color << 32) | mCrumbs[Index].Color;
    *(UINT64 *)&T[TRAMP_STRIDE] = (UINT64)mFbStride * 4;

    *Code = EncodeB ((UINT64)(UINTN)Code, (UINT64)(UINTN)T);
    WriteBackDataCacheRange (Code, 4);
    InvalidateInstructionCacheRange (Code, 4);
    EMIT ("T> crumb %d: %a\n", (UINT32)(Index + 1), mCrumbs[Index].Name);
  }

  WriteBackDataCacheRange ((VOID *)(UINTN)Tramp, EFI_PAGE_SIZE);
  InvalidateInstructionCacheRange ((VOID *)(UINTN)Tramp, EFI_PAGE_SIZE);
}

//
// Experiment switch: if \r8s-noacpi exists on the ESP, remove the ACPI
// configuration tables before Boot Manager starts, to see whether it gets
// past BlUtlGetAcpiTable('FACP') without them.
//
STATIC
VOID
MaybeHideAcpi (
  IN EFI_HANDLE  Device
  )
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;

  if (EFI_ERROR (gBS->HandleProtocol (Device, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) ||
      EFI_ERROR (Fs->OpenVolume (Fs, &Root)))
  {
    return;
  }

  if (!EFI_ERROR (Root->Open (Root, &File, L"\\r8s-noacpi", EFI_FILE_MODE_READ, 0))) {
    File->Close (File);
    EMIT ("T> r8s-noacpi present: removing ACPI tables: 2.0 %r, 1.0 %r\n",
          gBS->InstallConfigurationTable (&gEfiAcpi20TableGuid, NULL),
          gBS->InstallConfigurationTable (&gEfiAcpi10TableGuid, NULL));
  }

  Root->Close (Root);
}

STATIC
VOID
UpdateCrcs (
  VOID
  )
{
  UINT32  Crc;

  gBS->Hdr.CRC32 = 0;
  mBs.CalculateCrc32 ((UINT8 *)gBS, gBS->Hdr.HeaderSize, &Crc);
  gBS->Hdr.CRC32 = Crc;

  gRT->Hdr.CRC32 = 0;
  mBs.CalculateCrc32 ((UINT8 *)gRT, gRT->Hdr.HeaderSize, &Crc);
  gRT->Hdr.CRC32 = Crc;
}

STATIC
VOID
RestoreTables (
  VOID
  )
{
  mActive = FALSE;
  CopyMem (gBS, &mBs, sizeof (mBs));
  CopyMem (gRT, &mRt, sizeof (mRt));
  UpdateCrcs ();
}

// ----------------------------------------------------------- Boot Services --

STATIC EFI_STATUS EFIAPI
TAllocatePages (EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemType, UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory)
{
  EFI_STATUS Status;
  T ("AllocatePages", "type %d mem %d pages 0x%lx addr 0x%lx", Type, MemType, (UINT64)Pages, (Memory != NULL) ? *Memory : 0);
  Status = mBs.AllocatePages (Type, MemType, Pages, Memory);
  if (mActive && FromBootmgr (CALLER)) {
    if (!EFI_ERROR (Status) && (MemType == EfiLoaderCode) && (Memory != NULL)) {
      AddRange (*Memory, EFI_PAGES_TO_SIZE (Pages));
    }

    if (!mInLog && mRepeat == 1) {
      EMIT ("   = %r 0x%lx\n", Status, (Memory != NULL) ? *Memory : 0);
    }
  }
  return Status;
}

STATIC EFI_STATUS EFIAPI
TFreePages (EFI_PHYSICAL_ADDRESS Memory, UINTN Pages)
{
  T ("FreePages", "0x%lx pages 0x%lx", Memory, (UINT64)Pages);
  return mBs.FreePages (Memory, Pages);
}

STATIC EFI_STATUS EFIAPI
TGetMemoryMap (UINTN *Size, EFI_MEMORY_DESCRIPTOR *Map, UINTN *Key, UINTN *DescSize, UINT32 *DescVer)
{
  EFI_STATUS Status;
  T ("GetMemoryMap", "size %d", (Size != NULL) ? (UINT32)*Size : 0);
  Status = mBs.GetMemoryMap (Size, Map, Key, DescSize, DescVer);
  R ("GetMemoryMap", Status);
  return Status;
}

STATIC EFI_STATUS EFIAPI
TAllocatePool (EFI_MEMORY_TYPE PoolType, UINTN Size, VOID **Buffer)
{
  T ("AllocatePool", "type %d size 0x%lx", PoolType, (UINT64)Size);
  return mBs.AllocatePool (PoolType, Size, Buffer);
}

STATIC EFI_STATUS EFIAPI
TFreePool (VOID *Buffer)
{
  T ("FreePool", "%p", Buffer);
  return mBs.FreePool (Buffer);
}

STATIC EFI_STATUS EFIAPI
TCreateEvent (UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY Fn, VOID *Ctx, EFI_EVENT *Event)
{
  T ("CreateEvent", "type 0x%x tpl %d", Type, (UINT32)Tpl);
  return mBs.CreateEvent (Type, Tpl, Fn, Ctx, Event);
}

STATIC EFI_STATUS EFIAPI
TCreateEventEx (UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY Fn, CONST VOID *Ctx, CONST EFI_GUID *Group, EFI_EVENT *Event)
{
  T ("CreateEventEx", "type 0x%x group %g", Type, Group);
  return mBs.CreateEventEx (Type, Tpl, Fn, Ctx, Group, Event);
}

STATIC EFI_STATUS EFIAPI
TSetTimer (EFI_EVENT Event, EFI_TIMER_DELAY Type, UINT64 Time)
{
  T ("SetTimer", "type %d time %ld", Type, Time);
  return mBs.SetTimer (Event, Type, Time);
}

STATIC EFI_STATUS EFIAPI
TWaitForEvent (UINTN Num, EFI_EVENT *Event, UINTN *Index)
{
  EFI_STATUS Status;
  T ("WaitForEvent", "n %d", (UINT32)Num);
  Status = mBs.WaitForEvent (Num, Event, Index);
  R ("WaitForEvent", Status);
  return Status;
}

STATIC EFI_STATUS EFIAPI
TSignalEvent (EFI_EVENT Event)
{
  T ("SignalEvent", "%p", Event);
  return mBs.SignalEvent (Event);
}

STATIC EFI_STATUS EFIAPI
TCloseEvent (EFI_EVENT Event)
{
  T ("CloseEvent", "%p", Event);
  return mBs.CloseEvent (Event);
}

STATIC EFI_STATUS EFIAPI
TCheckEvent (EFI_EVENT Event)
{
  T ("CheckEvent", "%p", Event);
  return mBs.CheckEvent (Event);
}

STATIC EFI_STATUS EFIAPI
TInstallProtocolInterface (EFI_HANDLE *Handle, EFI_GUID *Protocol, EFI_INTERFACE_TYPE Type, VOID *Interface)
{
  T ("InstallProtocolInterface", "%g", Protocol);
  return mBs.InstallProtocolInterface (Handle, Protocol, Type, Interface);
}

STATIC EFI_STATUS EFIAPI
TReinstallProtocolInterface (EFI_HANDLE Handle, EFI_GUID *Protocol, VOID *Old, VOID *New)
{
  T ("ReinstallProtocolInterface", "%g", Protocol);
  return mBs.ReinstallProtocolInterface (Handle, Protocol, Old, New);
}

STATIC EFI_STATUS EFIAPI
TUninstallProtocolInterface (EFI_HANDLE Handle, EFI_GUID *Protocol, VOID *Interface)
{
  T ("UninstallProtocolInterface", "%g", Protocol);
  return mBs.UninstallProtocolInterface (Handle, Protocol, Interface);
}

STATIC EFI_STATUS EFIAPI
THandleProtocol (EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface)
{
  EFI_STATUS Status;
  T ("HandleProtocol", "%p %g", Handle, Protocol);
  Status = mBs.HandleProtocol (Handle, Protocol, Interface);
  R ("HandleProtocol", Status);
  return Status;
}

STATIC EFI_STATUS EFIAPI
TRegisterProtocolNotify (EFI_GUID *Protocol, EFI_EVENT Event, VOID **Registration)
{
  T ("RegisterProtocolNotify", "%g", Protocol);
  return mBs.RegisterProtocolNotify (Protocol, Event, Registration);
}

STATIC EFI_STATUS EFIAPI
TLocateHandle (EFI_LOCATE_SEARCH_TYPE Search, EFI_GUID *Protocol, VOID *Key, UINTN *Size, EFI_HANDLE *Buffer)
{
  EFI_STATUS Status;
  T ("LocateHandle", "search %d %g", Search, Protocol);
  Status = mBs.LocateHandle (Search, Protocol, Key, Size, Buffer);
  R ("LocateHandle", Status);
  return Status;
}

STATIC EFI_STATUS EFIAPI
TLocateDevicePath (EFI_GUID *Protocol, EFI_DEVICE_PATH_PROTOCOL **DevicePath, EFI_HANDLE *Device)
{
  EFI_STATUS Status;
  T ("LocateDevicePath", "%g", Protocol);
  Status = mBs.LocateDevicePath (Protocol, DevicePath, Device);
  R ("LocateDevicePath", Status);
  return Status;
}

STATIC EFI_STATUS EFIAPI
TInstallConfigurationTable (EFI_GUID *Guid, VOID *Table)
{
  T ("InstallConfigurationTable", "%g", Guid);
  return mBs.InstallConfigurationTable (Guid, Table);
}

STATIC EFI_STATUS EFIAPI
TLoadImage (BOOLEAN Policy, EFI_HANDLE Parent, EFI_DEVICE_PATH_PROTOCOL *Path, VOID *Src, UINTN SrcSize, EFI_HANDLE *Image)
{
  EFI_STATUS Status;
  T ("LoadImage", "src %p size 0x%lx", Src, (UINT64)SrcSize);
  Status = mBs.LoadImage (Policy, Parent, Path, Src, SrcSize, Image);
  R ("LoadImage", Status);
  return Status;
}

STATIC EFI_STATUS EFIAPI
TStartImage (EFI_HANDLE Image, UINTN *ExitDataSize, CHAR16 **ExitData)
{
  T ("StartImage", "%p", Image);
  return mBs.StartImage (Image, ExitDataSize, ExitData);
}

STATIC EFI_STATUS EFIAPI
TExit (EFI_HANDLE Image, EFI_STATUS ExitStatus, UINTN ExitDataSize, CHAR16 *ExitData)
{
  T ("Exit", "%r", ExitStatus);
  return mBs.Exit (Image, ExitStatus, ExitDataSize, ExitData);
}

STATIC EFI_STATUS EFIAPI
TUnloadImage (EFI_HANDLE Image)
{
  T ("UnloadImage", "%p", Image);
  return mBs.UnloadImage (Image);
}

STATIC EFI_STATUS EFIAPI
TExitBootServices (EFI_HANDLE Image, UINTN MapKey)
{
  EMIT ("T> ExitBootServices key 0x%lx (tracer off, log closed)\n", (UINT64)MapKey);
  LogClose ();
  HeartbeatStop ();
  RestoreTables ();
  return gBS->ExitBootServices (Image, MapKey);
}

STATIC EFI_STATUS EFIAPI
TGetNextMonotonicCount (UINT64 *Count)
{
  T ("GetNextMonotonicCount", "", 0);
  return mBs.GetNextMonotonicCount (Count);
}

STATIC EFI_STATUS EFIAPI
TStall (UINTN Us)
{
  T ("Stall", "%d us", (UINT32)Us);
  return mBs.Stall (Us);
}

STATIC EFI_STATUS EFIAPI
TSetWatchdogTimer (UINTN Timeout, UINT64 Code, UINTN DataSize, CHAR16 *Data)
{
  T ("SetWatchdogTimer", "%d s", (UINT32)Timeout);
  return mBs.SetWatchdogTimer (Timeout, Code, DataSize, Data);
}

STATIC EFI_STATUS EFIAPI
TConnectController (EFI_HANDLE Ctrl, EFI_HANDLE *Driver, EFI_DEVICE_PATH_PROTOCOL *Remaining, BOOLEAN Recursive)
{
  T ("ConnectController", "%p", Ctrl);
  return mBs.ConnectController (Ctrl, Driver, Remaining, Recursive);
}

STATIC EFI_STATUS EFIAPI
TDisconnectController (EFI_HANDLE Ctrl, EFI_HANDLE Driver, EFI_HANDLE Child)
{
  T ("DisconnectController", "%p", Ctrl);
  return mBs.DisconnectController (Ctrl, Driver, Child);
}

STATIC EFI_STATUS EFIAPI
TOpenProtocol (EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface, EFI_HANDLE Agent, EFI_HANDLE Ctrl, UINT32 Attr)
{
  EFI_STATUS Status;
  T ("OpenProtocol", "%p %g attr 0x%x", Handle, Protocol, Attr);
  Status = mBs.OpenProtocol (Handle, Protocol, Interface, Agent, Ctrl, Attr);
  R ("OpenProtocol", Status);
  return Status;
}

STATIC EFI_STATUS EFIAPI
TCloseProtocol (EFI_HANDLE Handle, EFI_GUID *Protocol, EFI_HANDLE Agent, EFI_HANDLE Ctrl)
{
  T ("CloseProtocol", "%g", Protocol);
  return mBs.CloseProtocol (Handle, Protocol, Agent, Ctrl);
}

STATIC EFI_STATUS EFIAPI
TProtocolsPerHandle (EFI_HANDLE Handle, EFI_GUID ***Buffer, UINTN *Count)
{
  T ("ProtocolsPerHandle", "%p", Handle);
  return mBs.ProtocolsPerHandle (Handle, Buffer, Count);
}

STATIC EFI_STATUS EFIAPI
TLocateHandleBuffer (EFI_LOCATE_SEARCH_TYPE Search, EFI_GUID *Protocol, VOID *Key, UINTN *Count, EFI_HANDLE **Buffer)
{
  EFI_STATUS Status;
  T ("LocateHandleBuffer", "search %d %g", Search, Protocol);
  Status = mBs.LocateHandleBuffer (Search, Protocol, Key, Count, Buffer);
  R ("LocateHandleBuffer", Status);
  return Status;
}

STATIC EFI_STATUS EFIAPI
TLocateProtocol (EFI_GUID *Protocol, VOID *Registration, VOID **Interface)
{
  EFI_STATUS Status;
  T ("LocateProtocol", "%g", Protocol);
  Status = mBs.LocateProtocol (Protocol, Registration, Interface);
  R ("LocateProtocol", Status);
  return Status;
}

// -------------------------------------------------------- Runtime Services --

STATIC EFI_STATUS EFIAPI
TGetTime (EFI_TIME *Time, EFI_TIME_CAPABILITIES *Caps)
{
  T ("RT.GetTime", "", 0);
  return mRt.GetTime (Time, Caps);
}

STATIC EFI_STATUS EFIAPI
TSetTime (EFI_TIME *Time)
{
  T ("RT.SetTime", "", 0);
  return mRt.SetTime (Time);
}

STATIC EFI_STATUS EFIAPI
TGetVariable (CHAR16 *Name, EFI_GUID *Vendor, UINT32 *Attr, UINTN *Size, VOID *Data)
{
  EFI_STATUS Status;
  T ("RT.GetVariable", "%s", Name);
  Status = mRt.GetVariable (Name, Vendor, Attr, Size, Data);
  R ("RT.GetVariable", Status);
  return Status;
}

STATIC EFI_STATUS EFIAPI
TGetNextVariableName (UINTN *Size, CHAR16 *Name, EFI_GUID *Vendor)
{
  T ("RT.GetNextVariableName", "", 0);
  return mRt.GetNextVariableName (Size, Name, Vendor);
}

STATIC EFI_STATUS EFIAPI
TSetVariable (CHAR16 *Name, EFI_GUID *Vendor, UINT32 Attr, UINTN Size, VOID *Data)
{
  EFI_STATUS Status;
  T ("RT.SetVariable", "%s attr 0x%x size %d", Name, Attr, (UINT32)Size);
  Status = mRt.SetVariable (Name, Vendor, Attr, Size, Data);
  R ("RT.SetVariable", Status);
  return Status;
}

STATIC EFI_STATUS EFIAPI
TQueryVariableInfo (UINT32 Attr, UINT64 *Max, UINT64 *Remaining, UINT64 *MaxVar)
{
  T ("RT.QueryVariableInfo", "attr 0x%x", Attr);
  return mRt.QueryVariableInfo (Attr, Max, Remaining, MaxVar);
}

STATIC VOID EFIAPI
TResetSystem (EFI_RESET_TYPE Type, EFI_STATUS Status, UINTN Size, VOID *Data)
{
  T ("RT.ResetSystem", "type %d %r", Type, Status);
  mRt.ResetSystem (Type, Status, Size, Data);
}

// ------------------------------------------------------------------ main --

STATIC
VOID
InstallTracer (
  VOID
  )
{
  CopyMem (&mBs, gBS, sizeof (mBs));
  CopyMem (&mRt, gRT, sizeof (mRt));

  gBS->AllocatePages              = TAllocatePages;
  gBS->FreePages                  = TFreePages;
  gBS->GetMemoryMap               = TGetMemoryMap;
  gBS->AllocatePool               = TAllocatePool;
  gBS->FreePool                   = TFreePool;
  gBS->CreateEvent                = TCreateEvent;
  gBS->CreateEventEx              = TCreateEventEx;
  gBS->SetTimer                   = TSetTimer;
  gBS->WaitForEvent               = TWaitForEvent;
  gBS->SignalEvent                = TSignalEvent;
  gBS->CloseEvent                 = TCloseEvent;
  gBS->CheckEvent                 = TCheckEvent;
  gBS->InstallProtocolInterface   = TInstallProtocolInterface;
  gBS->ReinstallProtocolInterface = TReinstallProtocolInterface;
  gBS->UninstallProtocolInterface = TUninstallProtocolInterface;
  gBS->HandleProtocol             = THandleProtocol;
  gBS->RegisterProtocolNotify     = TRegisterProtocolNotify;
  gBS->LocateHandle               = TLocateHandle;
  gBS->LocateDevicePath           = TLocateDevicePath;
  gBS->InstallConfigurationTable  = TInstallConfigurationTable;
  gBS->LoadImage                  = TLoadImage;
  gBS->StartImage                 = TStartImage;
  gBS->Exit                       = TExit;
  gBS->UnloadImage                = TUnloadImage;
  gBS->ExitBootServices           = TExitBootServices;
  gBS->GetNextMonotonicCount      = TGetNextMonotonicCount;
  gBS->Stall                      = TStall;
  gBS->SetWatchdogTimer           = TSetWatchdogTimer;
  gBS->ConnectController          = TConnectController;
  gBS->DisconnectController       = TDisconnectController;
  gBS->OpenProtocol               = TOpenProtocol;
  gBS->CloseProtocol              = TCloseProtocol;
  gBS->ProtocolsPerHandle         = TProtocolsPerHandle;
  gBS->LocateHandleBuffer         = TLocateHandleBuffer;
  gBS->LocateProtocol             = TLocateProtocol;

  gRT->GetTime                    = TGetTime;
  gRT->SetTime                    = TSetTime;
  gRT->GetVariable                = TGetVariable;
  gRT->GetNextVariableName        = TGetNextVariableName;
  gRT->SetVariable                = TSetVariable;
  gRT->QueryVariableInfo          = TQueryVariableInfo;
  gRT->ResetSystem                = TResetSystem;

  UpdateCrcs ();
}

EFI_STATUS
EFIAPI
R8sTraceMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                 Status;
  EFI_LOADED_IMAGE_PROTOCOL  *Self;
  EFI_DEVICE_PATH_PROTOCOL   *Path;
  EFI_HANDLE                 Child;

  Print (L"\n=== r8s-woa tracer: starting Boot Manager with firmware-call trace ===\n");

  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Self);
  if (EFI_ERROR (Status)) {
    Print (L"LoadedImage: %r\n", Status);
    return Status;
  }

  Path = FileDevicePath (Self->DeviceHandle, L"\\efi\\boot\\winpe.efi");
  if (Path == NULL) {
    Print (L"FileDevicePath failed\n");
    return EFI_OUT_OF_RESOURCES;
  }

  LogOpen (Self->DeviceHandle);
  HeartbeatStart ();
  InstallTracer ();
  mActive = TRUE;
  EMIT ("T> tracer on\n");
  MaybeHideAcpi (Self->DeviceHandle);

  Status = gBS->LoadImage (FALSE, ImageHandle, Path, NULL, 0, &Child);
  EMIT ("T> LoadImage winpe.efi: %r\n", Status);
  if (!EFI_ERROR (Status)) {
    EFI_LOADED_IMAGE_PROTOCOL  *Img;

    if (!EFI_ERROR (gBS->HandleProtocol (Child, &gEfiLoadedImageProtocolGuid, (VOID **)&Img))) {
      AddRange ((UINT64)(UINTN)Img->ImageBase, Img->ImageSize);
      EMIT ("T> Boot Manager image at 0x%lx size 0x%lx\n", (UINT64)(UINTN)Img->ImageBase, Img->ImageSize);
      InstallCrumbs ((UINT64)(UINTN)Img->ImageBase, Img->ImageSize);
    }

    Status = gBS->StartImage (Child, NULL, NULL);
  }

  EMIT ("T> Boot Manager returned: %r\n", Status);
  HeartbeatStop ();
  RestoreTables ();
  LogClose ();
  Print (L"\nBoot Manager returned: %r\n", Status);
  return Status;
}
