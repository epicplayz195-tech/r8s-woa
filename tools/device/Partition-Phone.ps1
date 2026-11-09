<#
.SYNOPSIS
  Replace Android's userdata with an ESP and a Windows partition.

.DESCRIPTION
  Deletes "userdata" on its UFS LUN and creates, in the same space:
    esp  - FAT32, EspSizeMiB (default 512)
    win  - Microsoft basic data, the rest (formatted NTFS from Windows later)
  Destroys all Android data. Run Backup-Phone.ps1 first.
#>
param(
    [int]$EspSizeMiB = 512,
    [string]$BackupDir = "D:\r8s-woa\backups"
)
. "$PSScriptRoot\Common.ps1"

$null = Assert-R8sInRecovery
if (-not (Get-ChildItem -Path $BackupDir -Recurse -Filter "SHA256SUMS" -ErrorAction SilentlyContinue)) {
    throw "No backup found under $BackupDir. Run Backup-Phone.ps1 first."
}

$sgdisk = Invoke-PhoneShell "command -v sgdisk || true"
if (-not $sgdisk) { throw "sgdisk not found in this TWRP build. Push a static arm64 sgdisk to /tmp and add it to PATH." }

$node = Get-PartitionNode "userdata"           # e.g. /dev/block/sda31
if ($node -notmatch "^/dev/block/(sd[a-z])(\d+)$") { throw "Unexpected userdata node: $node" }
$disk = "/dev/block/$($Matches[1])"
$num  = [int]$Matches[2]

$info  = Invoke-PhoneShell "sgdisk -i $num $disk"
$first = [regex]::Match($info, "First sector: (\d+)").Groups[1].Value
$last  = [regex]::Match($info, "Last sector: (\d+)").Groups[1].Value
$secsz = [regex]::Match((Invoke-PhoneShell "sgdisk -p $disk"), "Sector size \(logical\): (\d+)").Groups[1].Value
if (-not $first -or -not $last -or -not $secsz) { throw "Could not read userdata geometry:`n$info" }

$espSectors = [int64]$EspSizeMiB * 1MB / [int64]$secsz
$espEnd = [int64]$first + $espSectors - 1
$numWin = $num + 1
$used = Invoke-PhoneShell "sgdisk -p $disk | awk 'NR>1 && `$1 ~ /^[0-9]+`$/ {print `$1}'"
while ((($used -split "`n") | ForEach-Object { $_.Trim() }) -contains "$numWin") { $numWin++ }

Write-Host "userdata = $disk partition $num, sectors $first-$last ($secsz B/sector)"
Write-Host "Plan: $num -> esp ($EspSizeMiB MiB), $numWin -> win (rest)"
Confirm-Danger "This deletes userdata (all Android data) on $disk."

Invoke-PhoneShell "umount /data 2>/dev/null; umount /sdcard 2>/dev/null; true" | Out-Null
Invoke-PhoneShell ("sgdisk -d $num " +
    "-n ${num}:${first}:${espEnd} -t ${num}:EF00 -c ${num}:esp " +
    "-n ${numWin}:$($espEnd + 1):${last} -t ${numWin}:0700 -c ${numWin}:win $disk") | Out-Null
Invoke-PhoneShell "sync; blockdev --rereadpt $disk 2>/dev/null; true" | Out-Null

$mkfat = Invoke-PhoneShell "command -v mkfs.fat || command -v mkfs.vfat || true"
if ($mkfat) {
    Invoke-PhoneShell "$mkfat -F 32 -n ESP /dev/block/by-name/esp" | Out-Null
    Write-Host "ESP formatted FAT32."
} else {
    Write-Warning "No mkfs.fat in TWRP: format 'esp' as FAT32 from Windows over UEFI Mass Storage."
}
Write-Host "Done. Format 'win' as NTFS from Windows (see docs/install.md)."
