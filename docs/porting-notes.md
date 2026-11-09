# Porting notes: r8s from x1s

Everything below was derived by diffing Mu-Silicium `Platforms/Samsung/r8sPkg`
against `x1sPkg` (both S5E9830) on 2026-11-09, plus the mainline Linux
`exynos990-r8s.dts`.

## What upstream r8s already had

- DSC/FDF/DeviceBuild.py, `Resources/Configs/r8s.toml` (FD at `0xE8000000`,
  size `0x200000`, boot image header v2, `Resources/DTBs/r8s.dtb`).
- SimpleFB at 1080×2400, framebuffer reserved at `0xF1000000`.
- Keypad: Vol+ GPA0[3], Vol− GPA0[4], Power GPA2[4] — matches the mainline DT.

The toml, DeviceBuild.py and KeypadDeviceLib are unchanged, so they are not
in the overlay.

## What the overlay changes

### DXE drivers (`Include/DXE.inc`, `Include/APRIORI.inc`)
Added, exactly as on x1s: `DiskIoDxe`, `PartitionDxe`, `UfsDxe`, `MshcDxe`,
`EnglishDxe`, `Fat`, `UsbfnDwc3Dxe`. Without them r8s UEFI has no storage and
no USB, which is why its status page shows Internal Storage / USB / Windows
Boot as ❌.

### PmicDxe removed
Found on a bit-24 S20 FE with a per-driver screen log (in the r8sios
project): DXE runs normally for ~4 ms, then stops inside `PmicDxe`. Its only
job is three SPEEDY writes that enable two PMIC protection features; on bit-24
firmware those transfers never complete (most likely the ACPM co-processor
owns the SPEEDY bus). Without PmicDxe the UEFI reaches the logo and boot menu.
`SpeedyDxe` itself is kept.

Side effect to watch: nothing in UEFI sets PMIC regulators now, so UFS and
microSD rely on the bootloader leaving their supplies on. UFS should be fine
(S-Boot loads the kernel from it); microSD may not power up.

Open item: one driver (#36 in the screen log) printed a red `ER` and carried
on. Not identified yet.

### ACPI (`r8s.dsc`, `r8s.fdf`, `Silicium-ACPI/Platforms/Samsung/r8s/AcpiTables.inf`)
Upstream had the ACPI component and FDF lines commented out. Enabled them and
added an `AcpiTables.inf` identical to x1s's (S5E9830 FADT/GTDT/MADT/DSDT/SSDT)
with a fresh `FILE_GUID`. Windows won't boot without ACPI.

### Memory map (`Library/MemoryMapLib/MemoryMapLib.c`)

| Region | Upstream r8s | Overlay | Why |
|---|---|---|---|
| EL2 Code 2 `0xC1400000` +2 MiB | — | added | x1s, same bootloader family |
| S2MPU Table `0xC1800000` +3 MiB | — | added | x1s |
| EL2 Early Memory | — | `0x9_3E800000` +24 MiB | see below |
| USB DRD PHY `0x10C00000`, Link `0x10E00000` | — | added | needed by UsbfnDwc3Dxe |
| CMU HSI1 `0x13000000` | — | added | UFS clocks |
| MSHC `0x132E0000` | — | added | microSD |
| UFS UniPro `0x13180000` | 0x1000 | 0x8000 | x1s size |
| SYSREG CMGP `0x15C20000` | — | added | x1s |

**EL2 Early Memory derivation.** On x1s it sits at `0xA_FE800000`. x1s has
12 GB; its last DRAM bank is `0x8_80000000 + 0x2_80000000`, ending at
`0xB_00000000`, so the region is "top of RAM − 0x1800000". The mainline r8s DT
gives 6 GB with the last bank `0x8_80000000 + 0xC0000000`, top `0x9_40000000`,
so the same rule gives **`0x9_3E800000`**. This is the single most likely thing
to be wrong. Verify with `cat /proc/iomem` from TWRP or the S-Boot log, and use
`0x9_BE800000` for an 8 GB unit.

## Things to verify on hardware (in order)

1. UEFI reaches the Silicium menu at all (if it hangs early: EL2/S2MPU regions).
2. Boot Manager lists UFS partitions → UfsDxe OK.
3. microSD shows up → MshcDxe OK (lower priority).
4. Mass Storage enumerates on the PC → DWC3 OK.
5. WinPE boots after erasing `uh`.

## Known gaps

- Only one CPU core in Windows on x1s; expect the same here (PSCI/MADT work).
- Display in Windows is the UEFI framebuffer only (no DECON driver).
- No Windows UFS driver for S5E9830 exists; full Windows can't see its own
  disk after ExitBootServices. See `driver-plan.md`.
