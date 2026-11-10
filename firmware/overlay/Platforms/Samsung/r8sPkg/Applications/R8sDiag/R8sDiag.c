/** @file
  r8s-woa firmware self-test.

  Calls, one at a time, the UEFI services Windows Boot Manager uses right at
  start-up, printing a line before each call and its result after. If the
  phone hangs, the last line on screen names the service that hangs.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/ArmLib.h>
#include <Library/ArmGenericTimerCounterLib.h>
#include <Protocol/Rng.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/BlockIo.h>
#include <Guid/GlobalVariable.h>
#include <Library/PrintLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

//
// Everything printed also goes to \r8s-diag.txt on the ESP, flushed per
// line, so the results can be pulled from TWRP even after a hang.
//
STATIC EFI_FILE_PROTOCOL  *mLog;

STATIC
VOID
LogPrint (
  IN CONST CHAR16  *Format,
  ...
  )
{
  VA_LIST  Args;
  CHAR16   Line[256];
  CHAR8    Ascii[256];
  UINTN    Size;
  UINTN    Index;

  VA_START (Args, Format);
  UnicodeVSPrint (Line, sizeof (Line), Format, Args);
  VA_END (Args);

  Print (L"%s", Line);
  if (mLog == NULL) {
    return;
  }

  for (Index = 0; Line[Index] != 0 && Index < ARRAY_SIZE (Ascii) - 1; Index++) {
    Ascii[Index] = (Line[Index] < 0x80) ? (CHAR8)Line[Index] : '?';
  }

  Ascii[Index] = 0;
  Size         = Index;
  mLog->Write (mLog, &Size, Ascii);
  mLog->Flush (mLog);
}

STATIC
VOID
LogOpen (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_LOADED_IMAGE_PROTOCOL        *Self;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;

  if (EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Self)) ||
      EFI_ERROR (gBS->HandleProtocol (Self->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) ||
      EFI_ERROR (Fs->OpenVolume (Fs, &Root)))
  {
    return;
  }

  if (!EFI_ERROR (Root->Open (Root, &File, L"\\r8s-diag.txt", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
    File->Delete (File);
  }

  if (EFI_ERROR (Root->Open (Root, &mLog, L"\\r8s-diag.txt", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) {
    mLog = NULL;
  }

  Root->Close (Root);
}

STATIC EFI_GUID mTestVarGuid = { 0x6a1f3c2e, 0x8b4d, 0x4e7a, { 0x9c, 0x51, 0x2d, 0x7e, 0x33, 0x10, 0xa8, 0x5f } };

#define STEP(Name)  LogPrint (L"%-34s ... ", Name)

STATIC
VOID
TestTimer (
  VOID
  )
{
  UINT64  Freq;
  UINT64  Before;
  UINT64  After;

  STEP (L"CurrentEL");
  LogPrint (L"EL%d\n", (UINT32)((ArmReadCurrentEL () >> 2) & 3));

  STEP (L"CNTFRQ_EL0");
  Freq = ArmGenericTimerGetTimerFreq ();
  LogPrint (L"%ld Hz%s\n", Freq, (Freq == 0) ? L"  (EMPTY)" : L"");

  STEP (L"Counter over Stall(100 ms)");
  Before = ArmGenericTimerGetSystemCount ();
  gBS->Stall (100000);
  After = ArmGenericTimerGetSystemCount ();
  LogPrint (L"+%ld ticks\n", After - Before);
}

STATIC
VOID
TestRuntime (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_TIME    Time;
  UINT16      BootCurrent;
  UINTN       Size;
  UINT32      Value;

  STEP (L"RT GetTime");
  Status = gRT->GetTime (&Time, NULL);
  LogPrint (L"%r  %04d-%02d-%02d %02d:%02d\n", Status, Time.Year, Time.Month, Time.Day, Time.Hour, Time.Minute);

  STEP (L"RT GetVariable BootCurrent");
  Size   = sizeof (BootCurrent);
  Status = gRT->GetVariable (L"BootCurrent", &gEfiGlobalVariableGuid, NULL, &Size, &BootCurrent);
  LogPrint (L"%r\n", Status);

  Value = 0x12345678;

  STEP (L"RT SetVariable (volatile)");
  Status = gRT->SetVariable (L"R8sDiagV", &mTestVarGuid, EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, sizeof (Value), &Value);
  LogPrint (L"%r\n", Status);

  STEP (L"RT SetVariable (non-volatile)");
  Status = gRT->SetVariable (L"R8sDiagNV", &mTestVarGuid, EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, sizeof (Value), &Value);
  LogPrint (L"%r\n", Status);

  STEP (L"RT SetVariable (delete)");
  gRT->SetVariable (L"R8sDiagV", &mTestVarGuid, 0, 0, NULL);
  Status = gRT->SetVariable (L"R8sDiagNV", &mTestVarGuid, 0, 0, NULL);
  LogPrint (L"%r\n", Status);
}

STATIC
VOID
TestRng (
  VOID
  )
{
  EFI_STATUS         Status;
  EFI_RNG_PROTOCOL   *Rng;
  EFI_RNG_ALGORITHM  Algos[8];
  UINTN              AlgoSize;
  UINT8              Buffer[64];

  STEP (L"Locate RNG protocol");
  Status = gBS->LocateProtocol (&gEfiRngProtocolGuid, NULL, (VOID **)&Rng);
  LogPrint (L"%r\n", Status);
  if (EFI_ERROR (Status)) {
    return;
  }

  STEP (L"RNG GetInfo");
  AlgoSize = sizeof (Algos);
  Status   = Rng->GetInfo (Rng, &AlgoSize, Algos);
  LogPrint (L"%r\n", Status);

  STEP (L"RNG GetRNG 16 bytes");
  Status = Rng->GetRNG (Rng, NULL, 16, Buffer);
  LogPrint (L"%r  %02x%02x%02x%02x...\n", Status, Buffer[0], Buffer[1], Buffer[2], Buffer[3]);

  STEP (L"RNG GetRNG 64 bytes");
  Status = Rng->GetRNG (Rng, NULL, 64, Buffer);
  LogPrint (L"%r\n", Status);
}

STATIC
VOID
TestGraphics (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;

  STEP (L"Locate GOP");
  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  LogPrint (L"%r\n", Status);
  if (EFI_ERROR (Status)) {
    return;
  }

  LogPrint (L"    mode %d/%d, %dx%d, format %d, fb 0x%lx size 0x%lx\n",
         Gop->Mode->Mode, Gop->Mode->MaxMode,
         Gop->Mode->Info->HorizontalResolution, Gop->Mode->Info->VerticalResolution,
         Gop->Mode->Info->PixelFormat, Gop->Mode->FrameBufferBase, (UINT64)Gop->Mode->FrameBufferSize);

  STEP (L"GOP SetMode (same mode)");
  Status = Gop->SetMode (Gop, Gop->Mode->Mode);
  LogPrint (L"%r\n", Status);
}

STATIC
VOID
TestMemory (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR  *Map;
  EFI_MEMORY_DESCRIPTOR  *Desc;
  UINTN                  MapSize;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVer;
  UINT64                 Total;
  UINT64                 Largest;
  UINTN                  Index;
  EFI_PHYSICAL_ADDRESS   Addr = 0;

  STEP (L"GetMemoryMap");
  MapSize = 0;
  Map     = NULL;
  Status  = gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescSize, &DescVer);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    MapSize += 8 * DescSize;
    Map      = AllocatePool (MapSize);
    Status   = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVer);
  }

  LogPrint (L"%r\n", Status);
  if (EFI_ERROR (Status)) {
    return;
  }

  Total   = 0;
  Largest = 0;
  for (Index = 0; Index < MapSize / DescSize; Index++) {
    Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Index * DescSize);
    if (Desc->Type == EfiConventionalMemory) {
      Total += Desc->NumberOfPages;
      if (Desc->NumberOfPages > Largest) {
        Largest = Desc->NumberOfPages;
      }
    }
  }

  LogPrint (L"    free RAM %ld MB, largest free block %ld MB, %d entries\n",
         Total * EFI_PAGE_SIZE / 0x100000, Largest * EFI_PAGE_SIZE / 0x100000, (UINT32)(MapSize / DescSize));
  FreePool (Map);

  STEP (L"AllocatePages 512 MB");
  Status = gBS->AllocatePages (AllocateAnyPages, EfiLoaderData, EFI_SIZE_TO_PAGES (0x20000000), &Addr);
  LogPrint (L"%r  at 0x%lx\n", Status, Addr);
  if (!EFI_ERROR (Status)) {
    STEP (L"Touch + free 512 MB");
    SetMem ((VOID *)(UINTN)Addr, 0x20000000, 0xA5);
    gBS->FreePages (Addr, EFI_SIZE_TO_PAGES (0x20000000));
    LogPrint (L"OK\n");
  }
}

STATIC
VOID
TestBlockIo (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Count;
  EFI_HANDLE  *Handles;

  STEP (L"BlockIo handles");
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  LogPrint (L"%r  %d\n", Status, (UINT32)(EFI_ERROR (Status) ? 0 : Count));
  if (!EFI_ERROR (Status)) {
    FreePool (Handles);
  }
}

//
// System registers Windows Boot Manager writes when it switches to its own
// memory mapping. Each is written back with its current value (no effect on
// a normal CPU). If a hypervisor at EL2 (Samsung uh / harx) traps the write
// and never returns, the phone hangs right after the line naming it.
//
#define SYSREG_RW(Name)                                                          \
  STATIC UINT64 Read_##Name (VOID) { UINT64 V; __asm__ __volatile__ ("mrs %0, " #Name : "=r" (V)); return V; } \
  STATIC VOID Write_##Name (UINT64 V) { __asm__ __volatile__ ("msr " #Name ", %0\n isb" : : "r" (V) : "memory"); }

SYSREG_RW (ttbr0_el1)
SYSREG_RW (ttbr1_el1)
SYSREG_RW (tcr_el1)
SYSREG_RW (mair_el1)
SYSREG_RW (sctlr_el1)
SYSREG_RW (vbar_el1)
SYSREG_RW (contextidr_el1)

#define TEST_SYSREG(Name)                                             \
  do {                                                                \
    UINT64 V = Read_##Name ();                                        \
    LogPrint (L"%-34a ... ", "write " #Name " (same value)");            \
    Write_##Name (V);                                                 \
    LogPrint (L"OK  0x%lx\n", V);                                        \
  } while (0)

STATIC
VOID
TestLowMemory (
  VOID
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Addr;
  volatile UINT64       *P;
  UINTN                 Index;
  UINTN                 Bad;

  // Boot Manager puts its context-switch code here (0x80 pages at 0x80000000).
  Addr = 0x80000000;
  STEP (L"Allocate 0x80000000 (0x80 pages)");
  Status = gBS->AllocatePages (AllocateAddress, EfiLoaderCode, 0x80, &Addr);
  LogPrint (L"%r\n", Status);
  if (EFI_ERROR (Status)) {
    return;
  }

  STEP (L"Write + read back 512 KB there");
  P   = (volatile UINT64 *)(UINTN)Addr;
  Bad = 0;
  for (Index = 0; Index < 0x80000 / 8; Index++) {
    P[Index] = 0x5A5A0000ULL ^ Index;
  }

  for (Index = 0; Index < 0x80000 / 8; Index++) {
    if (P[Index] != (0x5A5A0000ULL ^ Index)) {
      Bad++;
    }
  }

  LogPrint (L"%d bad words\n", (UINT32)Bad);
  gBS->FreePages (Addr, 0x80);
}

STATIC
VOID
TestSysRegs (
  VOID
  )
{
  TEST_SYSREG (contextidr_el1);
  TEST_SYSREG (vbar_el1);
  TEST_SYSREG (mair_el1);
  TEST_SYSREG (tcr_el1);
  TEST_SYSREG (ttbr1_el1);
  TEST_SYSREG (ttbr0_el1);
  TEST_SYSREG (sctlr_el1);

  STEP (L"tlbi vmalle1 + dsb + isb");
  __asm__ __volatile__ ("dsb ishst\n tlbi vmalle1\n dsb ish\n isb" : : : "memory");
  LogPrint (L"OK\n");
}

EFI_STATUS
EFIAPI
R8sDiagMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  LogOpen (ImageHandle);
  LogPrint (L"\n=== r8s-woa firmware self-test ===\n");
  LogPrint (L"Firmware: %s rev 0x%x, UEFI %d.%d\n",
         SystemTable->FirmwareVendor, SystemTable->FirmwareRevision,
         SystemTable->Hdr.Revision >> 16, SystemTable->Hdr.Revision & 0xFFFF);

  TestTimer ();
  TestRuntime ();
  TestRng ();
  TestGraphics ();
  TestMemory ();
  TestBlockIo ();
  TestLowMemory ();
  TestSysRegs ();

  LogPrint (L"=== self-test finished: no hang ===\n\n");
  if (mLog != NULL) {
    mLog->Close (mLog);
  }

  return EFI_SUCCESS;
}
