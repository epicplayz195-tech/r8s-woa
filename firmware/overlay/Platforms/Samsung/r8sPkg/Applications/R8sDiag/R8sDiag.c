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

  // Full map: Boot Manager builds its page tables from these types/attributes.
  LogPrint (L"    --- memory map (type, start, end, pages, attributes) ---\n");
  for (Index = 0; Index < MapSize / DescSize; Index++) {
    Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Index * DescSize);
    LogPrint (L"    %2d  %2d  %09lx-%09lx  %8lx  %016lx\n",
              (UINT32)Index, (UINT32)Desc->Type, Desc->PhysicalStart,
              Desc->PhysicalStart + EFI_PAGES_TO_SIZE (Desc->NumberOfPages) - 1,
              Desc->NumberOfPages, Desc->Attribute);
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
SYSREG_RW (pmcr_el0)
SYSREG_RW (pmcntenclr_el0)
SYSREG_RW (pmcntenset_el0)
SYSREG_RW (pmovsclr_el0)
SYSREG_RW (pmintenclr_el1)
SYSREG_RW (pmccfiltr_el0)
SYSREG_RW (pmuserenr_el0)
SYSREG_RW (pmccntr_el0)
SYSREG_RW (cpacr_el1)

STATIC UINT64 Read_id_aa64dfr0_el1 (VOID) { UINT64 V; __asm__ __volatile__ ("mrs %0, id_aa64dfr0_el1" : "=r" (V)); return V; }

//
// Windows Boot Manager's BlpArchInitialize (bootmgfw 22621.1, rva 0x168838)
// programs the PMU right after its page allocator grabs memory and before
// its next firmware call. Replay that exact sequence, logging before every
// access, so a trapped/hanging PMU access names itself in r8s-diag.txt.
//
#define PMU_STEP(Text, Stmt)                         \
  do {                                               \
    LogPrint (L"    %-40a", Text);                   \
    Stmt;                                            \
    LogPrint (L"OK\n");                              \
  } while (0)

STATIC
VOID
TestPmu (
  VOID
  )
{
  UINT64  Dfr0;
  UINT64  Pmcr;
  UINT64  V;
  UINT64  C1;
  UINT64  C2;

  Dfr0 = Read_id_aa64dfr0_el1 ();
  LogPrint (L"PMU (Boot Manager's exact sequence)  ID_AA64DFR0 0x%lx, PMUVer %d\n", Dfr0, (UINT32)((Dfr0 >> 8) & 0xF));

  Pmcr = 0;
  PMU_STEP ("read PMCR_EL0", Pmcr = Read_pmcr_el0 ());
  LogPrint (L"    PMCR_EL0 = 0x%lx\n", Pmcr);

  V = Pmcr | 0x10;
  if ((V & 1) == 0) {
    V |= 5;
    PMU_STEP ("write PMCNTENCLR_EL0 = 0xffffffff", Write_pmcntenclr_el0 (0xFFFFFFFF));
    PMU_STEP ("write PMOVSCLR_EL0 = 0xffffffff", Write_pmovsclr_el0 (0xFFFFFFFF));
    PMU_STEP ("write PMINTENCLR_EL1 = 0xffffffff", Write_pmintenclr_el1 (0xFFFFFFFF));
  }

  PMU_STEP ("write PMCR_EL0", Write_pmcr_el0 (V & ~0x20ULL));
  PMU_STEP ("write PMCCFILTR_EL0 = 0x8000000", Write_pmccfiltr_el0 (0x8000000));
  PMU_STEP ("write PMCNTENSET_EL0 = 0x80000000", Write_pmcntenset_el0 (0x80000000));
  PMU_STEP ("write PMUSERENR_EL0 = 0", Write_pmuserenr_el0 (0));
  PMU_STEP ("read/write CPACR_EL1 |= 0x300000", Write_cpacr_el1 (Read_cpacr_el1 () | 0x300000));

  C1 = 0;
  C2 = 0;
  PMU_STEP ("read PMCCNTR_EL0 twice (1 ms apart)", C1 = Read_pmccntr_el0 (); gBS->Stall (1000); C2 = Read_pmccntr_el0 ());
  LogPrint (L"    cycle counter %lx -> %lx (%a)\n", C1, C2, (C2 != C1) ? "counting" : "NOT counting");
}

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

  //
  // Boot Manager loads NEW translation tables. Same mapping, different
  // address: copy the TTBR0 root table, switch to the copy, switch back.
  // A hypervisor that only allows tables it knows would hang here.
  //
  {
    EFI_PHYSICAL_ADDRESS  Copy;
    UINT64                Old;
    UINT64                Daif;

    STEP (L"TTBR0 -> copy of page tables");
    if (EFI_ERROR (gBS->AllocatePages (AllocateAnyPages, EfiBootServicesData, 1, &Copy))) {
      LogPrint (L"no memory\n");
    } else {
      Old = Read_ttbr0_el1 ();
      CopyMem ((VOID *)(UINTN)Copy, (VOID *)(UINTN)(Old & 0x0000FFFFFFFFF000ULL), EFI_PAGE_SIZE);
      __asm__ __volatile__ ("mrs %0, daif\n msr daifset, #0xf" : "=r" (Daif) : : "memory");
      __asm__ __volatile__ ("dsb ish" : : : "memory");
      Write_ttbr0_el1 (Copy | (Old & ~0x0000FFFFFFFFF000ULL));
      __asm__ __volatile__ ("tlbi vmalle1\n dsb ish\n isb" : : : "memory");
      Write_ttbr0_el1 (Old);
      __asm__ __volatile__ ("tlbi vmalle1\n dsb ish\n isb\n msr daif, %0" : : "r" (Daif) : "memory");
      gBS->FreePages (Copy, 1);
      LogPrint (L"OK  (0x%lx -> 0x%lx -> back)\n", Old, (UINT64)Copy);
    }
  }

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
  TestPmu ();

  LogPrint (L"=== self-test finished: no hang ===\n\n");
  if (mLog != NULL) {
    mLog->Close (mLog);
  }

  return EFI_SUCCESS;
}
