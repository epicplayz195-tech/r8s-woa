<#
.SYNOPSIS
  Flash Mu-r8s.img (Silicium UEFI) to the BOOT partition from TWRP.

.DESCRIPTION
  Checks the image is an Android boot image, pushes it, writes it to
  /dev/block/by-name/boot and reads it back to verify. Your original boot.img
  is in the Backup-Phone.ps1 folder if you need to go back.
#>
param(
    [Parameter(Mandatory)][string]$Image,
    [switch]$Reboot
)
. "$PSScriptRoot\Common.ps1"

$null = Assert-R8sInRecovery
if (-not (Test-Path $Image)) { throw "Image not found: $Image" }

$magic = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($Image)[0..7])
if ($magic -ne "ANDROID!") { throw "$Image is not an Android boot image (magic '$magic')." }

$bootSize = [int64](Invoke-PhoneShell "blockdev --getsize64 /dev/block/by-name/boot")
$imgSize  = (Get-Item $Image).Length
if ($imgSize -gt $bootSize) { throw "Image ($imgSize B) is larger than BOOT ($bootSize B)." }

$hash = (Get-FileHash $Image -Algorithm SHA256).Hash.ToLower()
Invoke-Adb push $Image /tmp/Mu-r8s.img | Out-Null
Invoke-PhoneShell "dd if=/tmp/Mu-r8s.img of=/dev/block/by-name/boot bs=4M; sync" | Out-Null

$readBack = Invoke-PhoneShell "head -c $imgSize /dev/block/by-name/boot | sha256sum | cut -d' ' -f1"
Invoke-PhoneShell "rm -f /tmp/Mu-r8s.img" | Out-Null
if ($readBack -ne $hash) { throw "Verification failed: wrote $hash, read back $readBack." }

Write-Host "UEFI flashed and verified ($hash)."
if ($Reboot) { Invoke-Adb reboot | Out-Null }
