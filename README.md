# r8s-woa: Windows on the Galaxy S20 FE (Exynos 990)

A Project Silicium (Mu-Silicium) based Windows on ARM port for the Samsung
Galaxy S20 FE, **SM-G780F, codename r8s, Exynos 990 / S5E9830**. It follows the
layout of [ntdevlabs/exynos9810-woa](https://github.com/ntdevlabs/exynos9810-woa)
and its fork [ilyatbn/exynos-woa](https://github.com/ilyatbn/exynos-woa)
(firmware + drivers + install tooling), but targets a different SoC, so very
little of their device code carries over directly.

> **Status: experimental, unvalidated.** Nothing here has been run on an r8s
> yet. Expect to debug. Back up first (`tools/device/Backup-Phone.ps1`).

## Where the port starts

Mu-Silicium already ships an r8s package, but it is marked *Inactive* with only
display and side buttons working. The Galaxy S20 5G (**x1s**) uses the same
S5E9830 SoC and is *Active*, with UFS, microSD, USB device mode and WinPE boot
working. The two packages are near-identical; r8s just never got x1s's
storage/USB/ACPI work. This repo grafts that work onto r8s:

| Change vs upstream r8sPkg | Source |
|---|---|
| UFS + microSD + FAT/partition DXE drivers | x1sPkg `DXE.inc` / `APRIORI.inc` |
| USB device mode (DWC3), used by FFU mode; no Mass Storage app on Samsung | x1sPkg |
| ACPI tables (FADT/GTDT/MADT/DSDT/SSDT) enabled | Silicium-ACPI x1s, new GUID |
| Memory map: 2nd EL2 code, S2MPU table, USB DRD, CMU HSI1, MSHC, CMGP, full UniPro window | x1sPkg `MemoryMapLib.c` |
| EL2 Early Memory moved to r8s's top of RAM (`0x9_3E800000`) | derived from mainline `exynos990-r8s.dts` |

Details and the things to verify on hardware: [docs/porting-notes.md](docs/porting-notes.md).

## Milestones

1. **UEFI with storage + USB** — boots to the Silicium menu, boots the UEFI Shell from UFS. *(UFS listed in the boot menu: confirmed on hardware)*
2. **WinPE boots** — what x1s reaches today. Needs `uh` erased.
3. **Full Windows** — needs a Windows UFS driver for Exynos 990 (port of `Exynos9810Ufs`), then HSI2C + touch. See [docs/driver-plan.md](docs/driver-plan.md).

## Repo layout

| Path | What |
|---|---|
| `firmware/overlay/` | Files copied over a Mu-Silicium checkout (r8sPkg + Silicium-ACPI) |
| `firmware/tools/build.sh` | Clone Mu-Silicium, apply overlay, build `Mu-r8s.img`, Odin tar and the ESP test image (Linux/WSL/CI) |
| `firmware/tools/make-esp.sh` | 1 GiB FAT32 ESP image with the UEFI Shell and a storage self-test |
| `firmware/tools/build.ps1` | Same, natively on Windows (defaults to `D:\woa`) |
| `.github/workflows/build-uefi.yml` | GitHub Actions build: `Mu-r8s.img` + an Odin tar (AP) |
| `tools/device/` | PowerShell + adb scripts run against TWRP: backup, erase `uh`, partition + test ESP, flash UEFI, copy files to the ESP |
| `tools/windows/Build-WinPE.ps1` | Make ARM64 WinPE media from a 23H2 ISO (or the ADK) |
| `tools/windows/Deploy-Windows.ps1` | Apply Windows to a disk the PC can see (for later; Samsung UEFI has no Mass Storage) |
| `drivers/` | Windows driver work for Exynos 990 (plans + hardware tables, no code yet) |
| `docs/` | Install guide, porting notes, driver plan |

## Quick start

Build (easiest: push this repo to GitHub, run **Actions → Build r8s UEFI**, download `Mu-r8s.img`).
Then follow [docs/install.md](docs/install.md).

## Requirements on the phone

- Unlocked bootloader, TWRP for r8s.
- Mu-Silicium lists **BIT-23** (bootloader binary revision) as required for r8s.
  This port also runs on **BIT-24**: there, upstream UEFI hangs in `PmicDxe`
  (its SPEEDY writes to the power chip never complete), so the overlay leaves
  PmicDxe out. With that change the UEFI reached the Silicium logo and boot
  menu on a bit-24 S20 FE.
  Check yours in TWRP: `adb shell getprop ro.boot.bootloader`; the binary
  revision is the 5th character from the end (letters continue after 9:
  A=10, … N=23, O=24).
- Erasing `uh` stops Android from booting until it's restored.

## Credits

Project Silicium / Mu-Silicium (Robotix22, halal-beef, Sota4Ever and
contributors) for the UEFI and the r8s/x1s packages; ntdev / ntdevlabs for the
exynos9810-woa structure and installer flow; ilyatbn for exynos-woa; Denzeel
Oliva (Sota4Ever) for the mainline r8s device tree; Microsoft Project Mu;
WOA-Project. See [NOTICE](NOTICE).

## License

BSD-2-Clause-Patent (see [LICENSE](LICENSE)). Code taken from Mu-Silicium keeps
its original license headers.
