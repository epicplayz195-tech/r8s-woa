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

Expected: Silicium logo, then the boot menu. Check:
1. Internal storage is listed (UfsDxe found the controller and the GPT).
2. Start **Mass Storage**, plug into the PC. If Windows offers to
   **Initialize** or **Format** a disk, click **Cancel** every time.
3. In an elevated PowerShell:
   ```powershell
   cd D:\woa\r8s-woa\tools\windows
   .\Test-PhoneStorage.ps1
   ```
   Read-only. It reads the GPT at both ends of the disk, re-reads 16 MiB at
   the start/middle/end and compares, and lists the partitions by name.
   All `[OK]` = UFS reads work across the whole disk through the UEFI.

If it hangs, note the last screen and see `porting-notes.md`.

## 3. Build WinPE (on the PC, elevated PowerShell)

```powershell
cd D:\woa\r8s-woa\tools\windows
.\Build-WinPE.ps1 -Iso D:\woa\iso\<your 22631 arm64>.iso
```
Output: `D:\woa\WinPE` (~400 MB).

## 4. Partition (phone in TWRP)

```powershell
cd D:\woa\r8s-woa\tools\device
.\Partition-Phone.ps1
```
Replaces `userdata` with `esp` (1 GiB FAT32) + `win` (the rest).

## 5. Erase uh (phone in TWRP)

```powershell
.\Erase-UH.ps1
```
Required for Windows: Samsung's hypervisor traps EL1 MMU registers.

## 6. Put WinPE on the phone

Boot the phone into the UEFI, start **Mass Storage**, plug it in. In Disk
Management give the phone's `esp` partition a letter (e.g. `S`). Then:
```powershell
cd D:\woa\r8s-woa\tools\windows
.\Deploy-Windows.ps1 -WinPE D:\woa\WinPE -Esp S
```
Eject, reboot the phone, pick the Windows Boot Manager entry. Success is a
WinPE command prompt on the phone's screen (no touch yet; that needs drivers).

## 7. Full Windows (later, once the UFS driver exists)

```powershell
.\Deploy-Windows.ps1 -Image D:\woa\iso\...\install.wim -Index 1 -Esp S -Win W -Drivers D:\woa\drivers
```

## Going back to Android

TWRP: restore `uh.img`, `boot.img`, `recovery.img` from the backup with `dd`,
restore the GPT (`sgdisk --load-backup` or write `gpt_sdX.bin`), or simply
flash the full stock firmware with Odin and then restore `efs` / `sec_efs`
from the backup.
