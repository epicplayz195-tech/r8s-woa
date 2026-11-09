# Installing on the Galaxy S20 FE (r8s)

> Destroys Android and its data. Read every step first.
> Everything on the PC lives in **`D:\woa`**; nothing goes on C:.

## Layout on the PC

| Folder | What |
|---|---|
| `D:\woa\r8s-woa` | this repo (scripts and docs) |
| `D:\woa\out` | `Mu-r8s.img` and `Mu-r8s-odin.tar.md5` from the GitHub Actions build |
| `D:\woa\iso` | your Windows 11 **23H2 ARM64** ISO |
| `D:\woa\WinPE` | WinPE media made by `Build-WinPE.ps1` |
| `D:\woa\backups` | phone backups made by `Backup-Phone.ps1` |
| `D:\woa\scratch` | DISM temp/mount space |

## 0. Prepare the PC

- Android platform-tools (`adb`) and the Samsung USB driver. Odin for the Download-mode route.
- A Windows 11 **23H2 (build 22631) ARM64** ISO, saved in `D:\woa\iso`.
  Get it from uupdump.net (Windows 11 23H2, arm64, "Download and convert to ISO").
  Do **not** use 24H2/25H2: Exynos only boots 23H2 and older. The same ISO
  gives you WinPE now and full Windows later.

## 1. Back up (phone in TWRP)

```powershell
cd D:\woa\r8s-woa\tools\device
.\Backup-Phone.ps1
```
Saves EFS (IMEI), boot, recovery, uh and every LUN's GPT to `D:\woa\backups`.
Copy that folder somewhere off the PC too.

If TWRP isn't on the phone right now (r8sios's recovery image replaced it),
flash TWRP back to RECOVERY first, or restore it from your old dump.

## 2. Flash the UEFI and test it (nothing destructive yet)

Either from TWRP:
```powershell
.\Flash-Uefi.ps1 -Image D:\woa\out\Mu-r8s.img -Reboot
```
or from Download mode with Odin: put `Mu-r8s-odin.tar.md5` in **AP** and start.
It only writes BOOT.

Expected: Silicium logo, then the boot menu, with internal storage listed
(UfsDxe found the controller and the GPT).

> Samsung builds of Silicium have **no Mass Storage** option (that app is
> Qualcomm-only). **FFU Mode** is something else (Microsoft's flashing
> protocol); don't use it. Files reach the phone through TWRP instead.

## 3. Partition and write the test ESP (phone in TWRP)

Needs `esp-r8s.img.gz` from the build in `D:\woa\out`.
```powershell
cd D:\woa\r8s-woa\tools\device
.\Partition-Phone.ps1
```
Replaces `userdata` with `esp` (1 GiB) + `win` (the rest), then writes a
FAT32 image to `esp` that holds the UEFI Shell and a self-test script, and
reads it back to verify. Reboot TWRP once afterwards.

## 4. Test storage in the UEFI

Boot the phone normally (UEFI), choose **internal storage** in the boot menu.
The UEFI Shell starts **from the phone's UFS** and runs the test by itself:
it lists the block devices and file systems, then prints the contents of the
ESP and the line `UFS + GPT + FAT32 read OK`.

- Shell appears and prints that line → UFS reads work. Type `reset` to reboot.
- Nothing happens / back to the menu → send a photo of the screen.

## 5. Build WinPE (on the PC, elevated PowerShell)

```powershell
cd D:\woa\r8s-woa\tools\windows
.\Build-WinPE.ps1 -Iso D:\woa\iso\<your 22631 arm64>.iso
```
Output: `D:\woa\WinPE` (~400 MB).

## 6. Erase uh (phone in TWRP)

```powershell
cd D:\woa\r8s-woa\tools\device
.\Erase-UH.ps1
```
Required for Windows: Samsung's hypervisor traps EL1 MMU registers.

## 7. Put WinPE on the phone (phone in TWRP)

```powershell
.\Push-Esp.ps1 -Source D:\woa\WinPE -Clean
```
Reboot to the UEFI, choose internal storage. Success is a WinPE command
prompt on the phone's screen (no touch yet; that needs drivers).

## 8. Full Windows (later, once the UFS driver exists)

Without Mass Storage, `Deploy-Windows.ps1` can't write to the phone directly.
Full Windows will be built into an image on the PC and written to `win`
through TWRP, the way exynos9810-woa does it; that script comes with the
UFS driver.

## Going back to Android

TWRP: restore `uh.img`, `boot.img`, `recovery.img` from the backup with `dd`,
restore the GPT (`sgdisk --load-backup` or write `gpt_sdX.bin`), or simply
flash the full stock firmware with Odin and then restore `efs` / `sec_efs`
from the backup.
