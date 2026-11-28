# r8s-woa: Windows on the Galaxy S20 FE (Exynos 990)

A Project Silicium (Mu-Silicium) based Windows on ARM port for the Samsung
Galaxy S20 FE, **SM-G780F, codename r8s, Exynos 990 / S5E9830**. It follows the
layout of [ntdevlabs/exynos9810-woa](https://github.com/ntdevlabs/exynos9810-woa)
and its fork [ilyatbn/exynos-woa](https://github.com/ilyatbn/exynos-woa)
(firmware + drivers + install tooling), but targets a different SoC, so very
little of their device code carries over directly.

> **Status: Windows 11 boots to the desktop from the phone's UFS** (28 Nov 2026, SM-G780F,
> bootloader bit 24, Windows 11 22H2 ARM64). Storage, touch (as a mouse) and restart work;
> one CPU core of eight, no USB/Wi-Fi/sound/GPU yet. Experimental: back up first.
>
> **Install guide: [docs/install.md](docs/install.md).** Firmware and drivers: **Releases**.

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

1. **UEFI with storage** - Silicium boots, the UEFI Shell runs from the `esp` on UFS. Done (9 Nov 2026).
2. **WinPE** - a desktop WinPE boots, with touch and the UFS driver. Done.
3. **Full Windows 11** - built on the PC, flashed through TWRP, set up unattended, boots to
   the desktop from UFS. Done (28 Nov 2026).
4. Next: the other 7 CPU cores, power-off, USB host, then the rest.

## Drivers (`drivers/`)

| Driver | Device | What |
|---|---|---|
| `r8sufs` | `ACPI\RWOA0004`, UFSHCI at 0x13100000 | Storport miniport. Takes over the controller UEFI left running (no link reset), one slot, polled, bounce buffer. Writes only inside the GPT partitions `esp` and `win`. Presents Samsung's GPT to Windows with HeaderSize 92 (Windows rejects Samsung's 512) and the Android partitions hidden. |
| `r8stouch` | `ACPI\RWOA0003`, Zinitix ZT7650 | KMDF HID minidriver, bit-banged I2C, polled. Absolute mouse by default, multi-touch mode available. |
| `r8slog` | `ACPI\RWOA0002`, reserved RAM | Windows -> firmware log across a warm restart (5 copies, majority vote), bugcheck codes, the UFS driver's command log; restarts through the PMU (no PSCI SYSTEM_RESET in Samsung's EL3). |

## Repo layout

| Path | What |
|---|---|
| `firmware/overlay/` | Files copied over a Mu-Silicium checkout (r8sPkg + Silicium-ACPI) |
| `firmware/tools/build.sh` | Clone Mu-Silicium, apply overlay, build `Mu-r8s.img`, Odin tar and the ESP test image (Linux/WSL/CI) |
| `firmware/tools/make-esp.sh` | 1 GiB FAT32 ESP image with the UEFI Shell and a storage self-test |
| `firmware/tools/build.ps1` | Same, natively on Windows (defaults to `D:\woa`) |
| `.github/workflows/build-uefi.yml` | GitHub Actions build: `Mu-r8s.img` + an Odin tar (AP) |
| `tools/device/` | PowerShell + adb scripts run against TWRP: backup/restore Android, flash the UEFI, erase `uh`, partition, flash Windows, push drivers, pull logs |
| `tools/windows/` | PC side: build the Windows image (`Build-WindowsImage.ps1`), desktop WinPE test (`Add-DesktopPe.ps1`) |
| `tools/pe/` | `r8sdisk.exe`, a disk-state tool for the WinPE test |
| `drivers/` | The Windows drivers above (built by Actions, test-signed) |
| `docs/` | Install guide, porting notes, touch notes |

## Quick start

Download the release, then follow [docs/install.md](docs/install.md). To build yourself:
**Actions -> Build r8s UEFI** and **Build Windows drivers (ARM64)**.

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
