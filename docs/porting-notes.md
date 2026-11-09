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
| EL2 Early Memory | — | `0x9_FEC00000` +20 MiB | phone's live DT `el2_earlymem` |
| USB DRD PHY `0x10C00000`, Link `0x10E00000` | — | added | needed by UsbfnDwc3Dxe |
| CMU HSI1 `0x13000000` | — | added | UFS clocks |
| MSHC `0x132E0000` | — | added | microSD |
| UFS UniPro `0x13180000` | 0x1000 | 0x8000 | x1s size |
| Display Reserved | 0x1194000 | 0x9E4000 | exact 1080×2400×4, as in r8sios |
| SYSREG CMGP `0x15C20000` | — | added | x1s |

**Verified on hardware.** The DRAM ranges, EL2 early memory and display size
come from the phone itself (r8sios `snapshot/reference/r8s`: `/proc/iomem` and
the live device tree), and this exact map (minus r8sios's iOS-only download
buffer at `0xF0000000`) boots to the Silicium menu on a bit-24 S20 FE.
RAM on this unit: `0x80000000-0xBAAFFFFF`, `0xC1200000-0xDFFFFFFF`,
`0xE1900000-0xFFFFFFFF`, `0x8_80000000-0x9_FFFFFFFF`.

## Things to verify on hardware (in order)

1. UEFI reaches the Silicium menu (already shown in r8sios with the same drivers and map).
2. Boot Manager lists UFS partitions → UfsDxe OK. r8sios's loader already reads and writes UFS through UfsDxe on this phone, so this is expected to work.
3. microSD shows up → MshcDxe OK (lower priority).
4. Mass Storage enumerates on the PC → DWC3 OK.
5. WinPE boots after erasing `uh`.

## Known gaps

- Only one CPU core in Windows on x1s; expect the same here (PSCI/MADT work).
- Display in Windows is the UEFI framebuffer only (no DECON driver).
- No Windows UFS driver for S5E9830 exists; full Windows can't see its own
  disk after ExitBootServices. See `driver-plan.md`.
