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

STATIC EFI_GUID mTestVarGuid = { 0x6a1f3c2e, 0x8b4d, 0x4e7a, { 0x9c, 0x51, 0x2d, 0x7e, 0x33, 0x10, 0xa8, 0x5f } };

#define STEP(Name)  Print (L"%-34s ... ", Name)

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
  Print (L"EL%d\n", (UINT32)((ArmReadCurrentEL () >> 2) & 3));

  STEP (L"CNTFRQ_EL0");
  Freq = ArmGenericTimerGetTimerFreq ();
  Print (L"%ld Hz%s\n", Freq, (Freq == 0) ? L"  (EMPTY)" : L"");

  STEP (L"Counter over Stall(100 ms)");
  Before = ArmGenericTimerGetSystemCount ();
  gBS->Stall (100000);
  After = ArmGenericTimerGetSystemCount ();
  Print (L"+%ld ticks\n", After - Before);
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
  Print (L"%r  %04d-%02d-%02d %02d:%02d\n", Status, Time.Year, Time.Month, Time.Day, Time.Hour, Time.Minute);

  STEP (L"RT GetVariable BootCurrent");
  Size   = sizeof (BootCurrent);
  Status = gRT->GetVariable (L"BootCurrent", &gEfiGlobalVariableGuid, NULL, &Size, &BootCurrent);
  Print (L"%r\n", Status);

  Value = 0x12345678;

  STEP (L"RT SetVariable (volatile)");
  Status = gRT->SetVariable (L"R8sDiagV", &mTestVarGuid, EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, sizeof (Value), &Value);
  Print (L"%r\n", Status);

  STEP (L"RT SetVariable (non-volatile)");
  Status = gRT->SetVariable (L"R8sDiagNV", &mTestVarGuid, EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, sizeof (Value), &Value);
  Print (L"%r\n", Status);

  STEP (L"RT SetVariable (delete)");
  gRT->SetVariable (L"R8sDiagV", &mTestVarGuid, 0, 0, NULL);
  Status = gRT->SetVariable (L"R8sDiagNV", &mTestVarGuid, 0, 0, NULL);
  Print (L"%r\n", Status);
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
  Print (L"%r\n", Status);
  if (EFI_ERROR (Status)) {
    return;
  }

  STEP (L"RNG GetInfo");
  AlgoSize = sizeof (Algos);
  Status   = Rng->GetInfo (Rng, &AlgoSize, Algos);
  Print (L"%r\n", Status);

  STEP (L"RNG GetRNG 16 bytes");
  Status = Rng->GetRNG (Rng, NULL, 16, Buffer);
  Print (L"%r  %02x%02x%02x%02x...\n", Status, Buffer[0], Buffer[1], Buffer[2], Buffer[3]);

  STEP (L"RNG GetRNG 64 bytes");
  Status = Rng->GetRNG (Rng, NULL, 64, Buffer);
  Print (L"%r\n", Status);
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
  Print (L"%r\n", Status);
  if (EFI_ERROR (Status)) {
    return;
  }

  Print (L"    mode %d/%d, %dx%d, format %d, fb 0x%lx size 0x%lx\n",
         Gop->Mode->Mode, Gop->Mode->MaxMode,
         Gop->Mode->Info->HorizontalResolution, Gop->Mode->Info->VerticalResolution,
         Gop->Mode->Info->PixelFormat, Gop->Mode->FrameBufferBase, (UINT64)Gop->Mode->FrameBufferSize);

  STEP (L"GOP SetMode (same mode)");
  Status = Gop->SetMode (Gop, Gop->Mode->Mode);
  Print (L"%r\n", Status);
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

  Print (L"%r\n", Status);
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

  Print (L"    free RAM %ld MB, largest free block %ld MB, %d entries\n",
         Total * EFI_PAGE_SIZE / 0x100000, Largest * EFI_PAGE_SIZE / 0x100000, (UINT32)(MapSize / DescSize));
  FreePool (Map);

  STEP (L"AllocatePages 512 MB");
  Status = gBS->AllocatePages (AllocateAnyPages, EfiLoaderData, EFI_SIZE_TO_PAGES (0x20000000), &Addr);
  Print (L"%r  at 0x%lx\n", Status, Addr);
  if (!EFI_ERROR (Status)) {
    STEP (L"Touch + free 512 MB");
    SetMem ((VOID *)(UINTN)Addr, 0x20000000, 0xA5);
    gBS->FreePages (Addr, EFI_SIZE_TO_PAGES (0x20000000));
    Print (L"OK\n");
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
  Print (L"%r  %d\n", Status, (UINT32)(EFI_ERROR (Status) ? 0 : Count));
  if (!EFI_ERROR (Status)) {
    FreePool (Handles);
  }
}

EFI_STATUS
EFIAPI
R8sDiagMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  Print (L"\n=== r8s-woa firmware self-test ===\n");
  Print (L"Firmware: %s rev 0x%x, UEFI %d.%d\n",
         SystemTable->FirmwareVendor, SystemTable->FirmwareRevision,
         SystemTable->Hdr.Revision >> 16, SystemTable->Hdr.Revision & 0xFFFF);

  TestTimer ();
  TestRuntime ();
  TestRng ();
  TestGraphics ();
  TestMemory ();
  TestBlockIo ();

  Print (L"=== self-test finished: no hang ===\n\n");
  return EFI_SUCCESS;
}
