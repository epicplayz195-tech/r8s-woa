# Installing Windows 11 on the Galaxy S20 FE (r8s)

Windows 11 ARM64, booted from the phone's own UFS storage, through Project Silicium's
UEFI. Tested on an **SM-G780F, bootloader G780FXXSOFYJ1 (bit 24)**, Windows 11 22H2
(22621) ARM64.

> **This wipes Android's `userdata`** (the space becomes `esp` + `win`). Back up first;
> `Restore-Android.ps1` puts the phone back exactly as it was.
> Everything on the PC lives in **`D:\woa`**. Nothing is written to C:.

## What works, what doesn't

| | |
|---|---|
| Boots from UFS, full Windows 11 desktop | yes |
| Storage (UFS, read/write) | yes: `r8sufs.sys` (polled, one command at a time; fine, not fast) |
| Touch screen | yes, as an absolute mouse (multi-touch mode is in the driver, untested in full Windows) |
| Restart | yes, through the PMU (`r8slog.sys`); Samsung's firmware has no PSCI `SYSTEM_RESET` |
| Shut down | restarts instead (power-off not done yet) |
| CPU | **1 core of 8** (the others fail to start: next on the list) |
| Graphics | firmware framebuffer, software rendering (no Mali GPU driver exists for Windows) |
| USB, Wi-Fi, Bluetooth, sound, cameras, modem, battery | no |
| Crash dumps | off (the UFS driver has no dump mode); a blue screen restarts after 15 s |

## What you need

- Unlocked bootloader and **TWRP** for r8s. Check the bootloader's binary revision in TWRP:
  `adb shell getprop ro.boot.bootloader`, 5th character from the end (`O` = 24).
- A Windows PC with ~40 GB free on **D:**, PowerShell 5.1 (built in), run **as Administrator**
  where noted.
- Android platform-tools in `D:\woa\r8s-woa\tools\device\platform-tools` (or `adb` on PATH),
  and Samsung's USB driver.
- A **Windows 11 ARM64 ISO, 23H2 or older** (22621 and 22631 work; 24H2 and newer don't boot on
  Exynos). From uupdump.net: Windows 11 22H2 or 23H2, arm64, "Download and convert to ISO".
  Put it anywhere under `D:\woa\iso`.
- From this repo's **Releases**: `Mu-r8s.img` (or `Mu-r8s-odin.tar.md5`), `esp-r8s.img.gz`,
  and `r8s-drivers-arm64.zip`.

## PC folders

| Folder | What |
|---|---|
| `D:\woa\r8s-woa` | this repo |
| `D:\woa\Mu-r8s-RELEASE` | `Mu-r8s.img`, `Mu-r8s-odin.tar.md5`, `esp-r8s.img.gz` |
| `D:\woa\drivers` | the drivers zip, unpacked (`r8slog\`, `r8stouch\`, `r8sufs\`, ...) |
| `D:\woa\iso` | the Windows ISO |
| `D:\woa\backups` | phone backups |
| `D:\woa\out\windows` | the Windows image built for the phone |
| `D:\woa\logs` | logs pulled from the phone |
| `D:\woa\scratch` | work space (the build's virtual disk, ~25 GB) |

All scripts are in `D:\woa\r8s-woa\tools\device` (phone side, over adb, phone in TWRP) and
`tools\windows` (PC side). Before long steps, turn off **QuickEdit** in the PowerShell
window (right-click the title bar, Properties): a click in the window pauses the script.

## 1. Back up (phone in TWRP)

```powershell
cd D:\woa\r8s-woa\tools\device
.\Backup-Phone.ps1      # EFS (IMEI), boot, recovery, uh, every GPT
.\Backup-Android.ps1    # every Android partition, checked; data copied sparse (only used blocks)
```
Keep `D:\woa\backups` somewhere else too. With the Android backup, everything (root,
modules, apps, settings) comes back on this phone with `Restore-Android.ps1`.

## 2. Flash the UEFI

From TWRP:
```powershell
.\Flash-Uefi.ps1 -Image D:\woa\Mu-r8s-RELEASE\Mu-r8s.img
```
or put `Mu-r8s-odin.tar.md5` in Odin's **AP** slot. Only BOOT is written; your boot.img is
in the backup.

## 3. Erase `uh` (phone in TWRP)

```powershell
.\Erase-UH.ps1
```
Samsung's hypervisor blocks Windows. Android won't boot until `uh` is restored
(`Restore-Android.ps1` does it).

## 4. Partition (phone in TWRP)

```powershell
.\Partition-Phone.ps1 -DryRun   # shows the plan, writes nothing
.\Partition-Phone.ps1
```
Replaces `userdata` with `esp` (1 GiB, FAT32, the UEFI Shell + self-test) and `win`
(the rest, ~107 GB). Both GPT copies are checked and read back. Reboot TWRP afterwards.

## 5. Build Windows for the phone (PC, Administrator, phone in TWRP)

Unzip `r8s-drivers-arm64.zip` into `D:\woa\drivers`, then:
```powershell
D:\woa\r8s-woa\tools\windows\Build-WindowsImage.ps1
```
15-30 minutes. It reads the phone's GPT (so the boot configuration can name the phone's
`esp` and `win`), then on a 4 KB-sector virtual disk:
- applies Windows 11 Pro from the ISO, adds the r8s drivers;
- writes an answer file: no setup screens, local account **User** / password **r8s**,
  signs in by itself (change it in Settings afterwards);
- turns off hibernation, fast startup and crash dumps;
- makes the boot files and the BCD (test signing on). The PC's `bcdboot` usually can't
  copy ARM64 boot files, so the script does it by hand and marks the BCD as a system store;
- saves `D:\woa\out\windows\win.sparse` (only blocks that hold data, ~10 GB) and the boot files.

Run it again with **`-Reuse`** to keep the already-applied Windows (a few minutes: drivers,
settings and boot files are redone).

## 6. Flash Windows (phone in TWRP)

```powershell
cd D:\woa\r8s-woa\tools\device
.\Flash-Windows.ps1
```
Type `YES`. Writes `win` (discard, then only the data blocks), reads all of it back to check,
and rewrites `esp`: the UEFI Shell, which saves the last Windows log, then starts Windows.
10-20 minutes. `-Direct` skips the Shell and boots Windows straight away.

## 7. First start

Reboot, choose **internal storage**. The Windows wheel appears over the Shell text, then
"Starting services", "Getting ready": **20-40+ minutes on one core**, with a restart.
If a restart stops at the Silicium menu, choose internal storage again. It ends on the
desktop, signed in as User.

## Updating the drivers in the installed Windows

Unzip the new drivers into `D:\woa\drivers`, phone in TWRP:
```powershell
.\Push-Drivers.ps1
```
Then in Windows, an Administrator command prompt (touch keyboard from the taskbar):
```
mountvol S: /S
S:\r8s-install-drivers.cmd
```
Accept "install anyway" if asked, restart.

## Logs

- **The firmware log**: Windows' last run (driver messages, the UFS command log, stop codes),
  kept in RAM across the restart and saved to the `esp` by the Shell. Restart into internal
  storage, wait for "Log of the last Windows run saved", go to TWRP, run `.\Pull-Logs.ps1`.
  Saved in `D:\woa\logs\<time>\r8s-winlog.txt`.
- **Windows' own logs** (setup, drivers, event logs): TWRP's NTFS driver can't read this
  partition (4 KB sectors with 1 KB MFT records), so copy it back with `.\Dump-Win.ps1`
  (~11 GB to `D:\woa\scratch\win-back.sparse`) and extract files from it.

## WinPE (optional, for testing)

A desktop WinPE (with Explorer and Device Manager) from `D:\woa\other-pe` can be booted
instead of Windows, with the drivers and a disk test: `tools\windows\Add-DesktopPe.ps1`
(Administrator), then `tools\device\Deploy-DesktopPe.ps1 -NewBootMgr` (phone in TWRP).
It replaces Windows' boot files on the `esp`; `Flash-Windows.ps1` puts them back.

## Going back to Android

```powershell
.\Restore-Android.ps1
```
The first run puts the original partition table back (`esp` and `win` go) and asks you to
reboot TWRP; the second writes every partition back and checks it, `uh` and `boot` included.
`efs` / `sec_efs` are only written if you name them with `-Partitions`.

## Troubleshooting

| Symptom | What it was / what to do |
|---|---|
| Reboots ~4 s after choosing internal storage | USB cable plugged in while the UEFI turns on USB-OTG power (fixed in the UEFI: it skips OTG when VBUS is present) |
| "Windows could not configure ... this computer's hardware" | the BCD was not a system store (fixed in `Build-WindowsImage.ps1`); rebuild with `-Reuse` and flash |
| Blue screen INTERNAL_POWER_ERROR on restart | old `r8slog.sys`: update the drivers |
| `Flash-Windows`: "input is not an r8s-sparse stream" | a byte-order mark from PowerShell's stdin (fixed); update the repo |
| Script seems stuck, title says "Select" | QuickEdit: press Enter |
| Stuck on the wheel for a long time on first start | normal on one core; give it 40+ minutes |
