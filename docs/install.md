# Installing on the Galaxy S20 FE (r8s)

> Destroys Android and its data. Read every step first.
> All PC-side files default to `D:\r8s-woa`.

## 0. Prepare the PC

- Android platform-tools (`winget install Google.PlatformTools`), Samsung USB driver.
- `Mu-r8s.img` from the GitHub Actions artifact or `firmware/tools/build.ps1`.
- ARM64 media: WinPE (Windows ADK + WinPE add-on, `copype arm64 D:\WinPE_arm64`)
  for milestone 2; a Windows 11 ARM64 `install.wim`/`.esd` (23H2 or older — the
  status page says newer builds don't boot on Exynos) for milestone 3.

## 1. Boot TWRP and back up

```powershell
cd D:\r8s-woa\repo\tools\device
.\Backup-Phone.ps1
```
Copy the backup folder somewhere off the PC too. It holds EFS (IMEI), boot,
recovery, uh and every LUN's GPT.

## 2. Flash and test the UEFI first (nothing destructive yet)

```powershell
.\Flash-Uefi.ps1 -Image D:\r8s-woa\out\Mu-r8s.img -Reboot
```
Expected: Silicium boot menu on screen, buttons navigate. Check that internal
storage shows up and try **Mass Storage** with the USB cable in. If it hangs or
shows no storage, stop here and see `porting-notes.md`. To go back: boot to
Download mode and flash `boot.img` from the backup.

## 3. Partition

Back in TWRP:
```powershell
.\Partition-Phone.ps1
```
Replaces `userdata` with `esp` (512 MiB) + `win` (the rest).

## 4. Erase uh

```powershell
.\Erase-UH.ps1
```
Required for Windows (Samsung's hypervisor traps EL1 MMU registers).

## 5. Deploy

Reboot to UEFI, start **Mass Storage**, assign letters in Disk Management
(e.g. `S` = esp, `W` = win), then in an elevated PowerShell:

```powershell
cd D:\r8s-woa\repo\tools\windows
# Milestone 2 – WinPE
.\Deploy-Windows.ps1 -WinPE D:\WinPE_arm64\media -Esp S
# Milestone 3 – full Windows (needs drivers that don't exist yet)
.\Deploy-Windows.ps1 -Image E:\sources\install.wim -Index 1 -Esp S -Win W -Drivers D:\r8s-woa\drivers
```

## Going back to Android

TWRP: restore `uh.img`, `boot.img`, `recovery.img` from the backup with `dd`,
restore the GPT (`sgdisk --load-backup` or write `gpt_sdX.bin`), or simply
flash the full stock firmware with Odin and then restore `efs` / `sec_efs`
from the backup.
