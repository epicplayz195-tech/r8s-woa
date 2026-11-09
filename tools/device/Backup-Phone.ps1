<#
.SYNOPSIS
  Back up everything you need to get the S20 FE back to stock, BEFORE any
  other r8s-woa step. Phone must be in TWRP.

.DESCRIPTION
  Pulls EFS/modem calibration, the partitions this project overwrites
  (boot, uh) and the GPT of every UFS LUN to your PC, and writes a SHA256
  manifest. Keep these files: they are how you restore IMEI, Android and
  the original partition table.
#>
param([string]$OutDir = "D:\woa\backups")
. "$PSScriptRoot\Common.ps1"

$bl = Assert-R8sInRecovery
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$dest = Join-Path $OutDir "$bl-$stamp"
New-Item -ItemType Directory -Force -Path $dest | Out-Null

# Device-unique data first, then what we overwrite.
$parts = @("efs", "sec_efs", "persistent", "param", "up_param", "boot", "recovery", "uh", "dtbo")

foreach ($p in $parts) {
    $exists = Invoke-PhoneShell "[ -e /dev/block/by-name/$p ] && echo yes || echo no"
    if ($exists -ne "yes") { Write-Host "skip $p (not present)"; continue }
    Write-Host "==> $p"
    Invoke-PhoneShell "dd if=/dev/block/by-name/$p of=/tmp/$p.img bs=4M" | Out-Null
    Invoke-Adb pull "/tmp/$p.img" (Join-Path $dest "$p.img") | Out-Null
    Invoke-PhoneShell "rm -f /tmp/$p.img" | Out-Null
}

# Partition tables of every UFS LUN (sda..sdf on Exynos 990).
foreach ($lun in "sda","sdb","sdc","sdd","sde","sdf") {
    $exists = Invoke-PhoneShell "[ -e /dev/block/$lun ] && echo yes || echo no"
    if ($exists -ne "yes") { continue }
    Write-Host "==> GPT $lun"
    # Primary GPT = first 6 sectors at 4 KiB UFS sector size (MBR + header + 4 entry blocks)
    Invoke-PhoneShell "dd if=/dev/block/$lun of=/tmp/gpt_$lun.bin bs=4096 count=6" | Out-Null
    Invoke-Adb pull "/tmp/gpt_$lun.bin" (Join-Path $dest "gpt_$lun.bin") | Out-Null
    Invoke-PhoneShell "rm -f /tmp/gpt_$lun.bin" | Out-Null
    $table = Invoke-PhoneShell "sgdisk -p /dev/block/$lun 2>/dev/null || true"
    Set-Content -Path (Join-Path $dest "gpt_$lun.txt") -Value $table
}

# Collect the file list first, so SHA256SUMS isn't hashed while it's being written.
$files = @(Get-ChildItem $dest -File | Where-Object Name -ne "SHA256SUMS")
$files | Get-FileHash -Algorithm SHA256 |
    ForEach-Object { "{0}  {1}" -f $_.Hash.ToLower(), (Split-Path $_.Path -Leaf) } |
    Set-Content (Join-Path $dest "SHA256SUMS")

Write-Host "`nBackup complete: $dest"
Write-Host "Copy this folder somewhere safe (cloud / second drive) before continuing."
