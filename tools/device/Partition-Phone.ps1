<#
.SYNOPSIS
  Replace Android's userdata with an ESP and a Windows partition.

.DESCRIPTION
  Deletes "userdata" on its UFS LUN and creates, in the same space:
    esp  - EspSizeMiB (default 1024, room for WinPE), then the prebuilt
           FAT32 image esp-r8s.img.gz (UEFI Shell + storage self-test) is
           written to it. TWRP usually has no mkfs.fat, hence the image.
    win  - Microsoft basic data, the rest (NTFS later)
  Destroys all Android data. Run Backup-Phone.ps1 first.
#>
param(
    [int]$EspSizeMiB = 1024,
    [string]$EspImage = "D:\woa\out\esp-r8s.img.gz",
    [string]$BackupDir = "D:\woa\backups",
    [string]$Scratch = "D:\woa\scratch"
)
. "$PSScriptRoot\Common.ps1"

$null = Assert-R8sInRecovery
if (-not (Get-ChildItem -Path $BackupDir -Recurse -Filter "SHA256SUMS" -ErrorAction SilentlyContinue)) {
    throw "No backup found under $BackupDir. Run Backup-Phone.ps1 first."
}

if (-not (Test-Path $EspImage) -and (Test-Path "D:\woa\Mu-r8s-RELEASE\esp-r8s.img.gz")) {
    $EspImage = "D:\woa\Mu-r8s-RELEASE\esp-r8s.img.gz"
}
if (-not (Test-Path $EspImage)) { throw "ESP image not found: $EspImage (it comes with the GitHub Actions build)." }
if ($EspSizeMiB -lt 1024) { throw "EspSizeMiB must be at least 1024 (the ESP image is 1 GiB)." }

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

# The kernel still has the old (larger) partition at the same start sector,
# so write through ${disk}${num} rather than the by-name link, which only
# updates after a reboot.
Write-Host "==> Writing the ESP image (FAT32 + UEFI Shell)"
New-Item -ItemType Directory -Force -Path $Scratch | Out-Null
$raw = Join-Path $Scratch "esp-r8s.img"
$in  = [System.IO.File]::OpenRead($EspImage)
$gz  = [System.IO.Compression.GZipStream]::new($in, [System.IO.Compression.CompressionMode]::Decompress)
$outf = [System.IO.File]::Create($raw)
try { $gz.CopyTo($outf) } finally { $outf.Dispose(); $gz.Dispose(); $in.Dispose() }
$rawSize = (Get-Item $raw).Length
if ($rawSize -gt [int64]$EspSizeMiB * 1MB) { throw "ESP image ($rawSize B) is larger than the ESP." }
$hash = (Get-FileHash $raw -Algorithm SHA256).Hash.ToLower()

Invoke-Adb push $raw /tmp/esp-r8s.img | Out-Null
Invoke-PhoneShell "dd if=/tmp/esp-r8s.img of=$disk$num bs=4M; sync" | Out-Null
$back = Invoke-PhoneShell "head -c $rawSize $disk$num | sha256sum | cut -d' ' -f1"
Invoke-PhoneShell "rm -f /tmp/esp-r8s.img" | Out-Null
Remove-Item $raw
if ($back -ne $hash) { throw "ESP verification failed: wrote $hash, read back $back." }

Write-Host "Done. ESP written and verified."
Write-Host "Reboot TWRP once so the new 'esp' and 'win' names appear."
Write-Host "Then flash/boot the UEFI and pick internal storage: the UEFI Shell should start from UFS."
