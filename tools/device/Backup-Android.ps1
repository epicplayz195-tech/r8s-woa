<#
.SYNOPSIS
  Full image backup of the phone's Android (ArtisanROM or anything else),
  so Restore-Android.ps1 can put it back exactly as it was. Phone in TWRP.

.DESCRIPTION
  Saves every partition the phone exposes by name as a raw image, gzip-
  compressed on the phone and streamed over USB to D:\woa\backups\android-*:
  boot (with root/KernelSU/Magisk), super (system/vendor/product), userdata
  (apps, settings, /data/adb modules such as Play Integrity fixes, your
  files), metadata (the encryption metadata userdata needs), uh, recovery,
  dtbo, vbmeta, EFS and the rest. It also saves the partition table of every
  UFS LUN.

  Each image is hashed on the phone while it is read, and then decompressed
  and hashed again on the PC, so a finished backup is known to be bit-exact.

  userdata is saved raw, still encrypted. That is what keeps everything
  intact, but it also means:
    - it only restores to THIS phone (the keys live in its security chip);
    - you can't open single files from it. Copy photos etc. separately.

  Free space on userdata compresses to almost nothing only if it was
  trimmed; the script runs fstrim first when TWRP has it.
#>
param(
    [string]$OutDir = "D:\woa\backups",
    [switch]$SkipPcVerify,
    [switch]$Force,
    [switch]$Fresh      # don't resume an unfinished backup; start a new folder
)
. "$PSScriptRoot\Common.ps1"
. "$PSScriptRoot\AdbStream.ps1"

$bl = Assert-R8sInRecovery
if ($OutDir -match '^[Cc]:') { throw "Refusing to write a multi-GB backup to C:. Use D:\woa\backups." }

$missing = Invoke-PhoneShell "for t in gzip mkfifo sha256sum tee dd blockdev; do command -v `$t >/dev/null 2>&1 || echo `$t; done"
if ($missing) { throw "TWRP is missing: $($missing -replace "`n", ', '). This TWRP build can't run the backup." }

# Resume the newest unfinished backup of this bootloader unless -Fresh.
$dest = $null
$done = @{}
if (-not $Fresh) {
    $dest = Get-ChildItem $OutDir -Directory -Filter "android-$bl-*" -ErrorAction SilentlyContinue |
        Where-Object { -not (Test-Path (Join-Path $_.FullName "manifest.json")) -and (Test-Path (Join-Path $_.FullName "progress.json")) } |
        Sort-Object Name | Select-Object -Last 1 -ExpandProperty FullName
}
if ($dest) {
    $list = Get-Content (Join-Path $dest "progress.json") -Raw | ConvertFrom-Json   # PS 5.1: assign first, so the array enumerates
    foreach ($r in $list) {
        $f = Join-Path $dest $r.File
        if ((Test-Path $f) -and (Get-Item $f).Length -eq [int64]$r.GzBytes) { $done[$r.Name] = $r }
    }
    Write-Host "Resuming $dest ($($done.Count) partitions already done and checked)." -ForegroundColor Cyan
} else {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $dest = Join-Path $OutDir "android-$bl-$stamp"
}
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Invoke-PhoneShell "getprop" | Set-Content (Join-Path $dest "getprop.txt")
Invoke-PhoneShell "ls -l /dev/block/by-name/" | Set-Content (Join-Path $dest "by-name.txt")

# ---- trim + size estimate (while /data is still mounted) ------------------
$mounted = Invoke-PhoneShell "grep -q ' /data ' /proc/mounts && echo yes || echo no"
$dataUsedKB = 0
if ($mounted -eq "yes") {
    $trim = Invoke-PhoneShell "(command -v fstrim >/dev/null && fstrim -v /data) 2>&1 || echo no-fstrim"
    Write-Host "fstrim /data: $trim"
    $df = Invoke-PhoneShell "df -k /data | tail -n 1"
    $f = $df.Trim() -split "\s+"
    if ($f.Count -ge 3 -and $f[2] -match '^\d+$') { $dataUsedKB = [int64]$f[2] }
    Write-Host ("/data in use: {0}" -f (Format-Size ($dataUsedKB * 1KB)))
} else {
    Write-Warning "/data isn't mounted in TWRP, so it can't be trimmed and its free space may not compress."
}

$still = Dismount-PhoneFilesystems
if ($still) { Write-Warning "Still mounted: $($still -join ', '). The backup should still be fine since TWRP isn't writing, but if a later restore won't boot, reboot TWRP, don't decrypt, and back up again." }

$parts = @(Get-PhonePartitions | Where-Object Size -gt 0 | Sort-Object Size)
if (-not ($parts | Where-Object Name -eq "userdata")) {
    if (-not $Force) { throw "No 'userdata' partition. If the phone is still partitioned for Windows (esp/win), Android isn't installed. Use -Force to back up anyway." }
}
$rawTotal = ($parts | Where-Object { -not $done.ContainsKey($_.Name) } | Measure-Object Size -Sum).Sum
$userdataSize = ($parts | Where-Object { $_.Name -eq "userdata" -and -not $done.ContainsKey($_.Name) } | Select-Object -First 1).Size
$estimate = $rawTotal - $userdataSize + ($dataUsedKB * 1KB) + 2GB
$drive = Get-PSDrive -Name ($dest.Substring(0, 1))
Write-Host ("{0} partitions, {1} raw. Expected backup size: about {2} or less. Free on {3}: {4}" -f `
    $parts.Count, (Format-Size $rawTotal), (Format-Size $estimate), $drive.Name, (Format-Size $drive.Free))
if ($drive.Free -lt $estimate -and -not $Force) { throw "Not enough free space on $($drive.Name):. Free some up or use -Force." }

# ---- partition tables ------------------------------------------------------
$luns = @()
foreach ($lun in "sda","sdb","sdc","sdd","sde","sdf","sdg","sdh") {
    if ((Invoke-PhoneShell "[ -b /dev/block/$lun ] && echo yes || echo no") -ne "yes") { continue }
    $ss = [int](Invoke-PhoneShell "blockdev --getss /dev/block/$lun")
    $bytes = [int64](Invoke-PhoneShell "blockdev --getsize64 /dev/block/$lun")
    $sectors = [int64]($bytes / $ss)
    $n = [int64][math]::Min(64, $sectors)
    foreach ($which in "head", "tail") {
        $skip = if ($which -eq "head") { 0 } else { $sectors - $n }
        Invoke-PhoneShell "dd if=/dev/block/$lun of=/tmp/r8s_gpt.bin bs=$ss skip=$skip count=$n 2>/dev/null" | Out-Null
        Invoke-Adb pull /tmp/r8s_gpt.bin (Join-Path $dest "gpt_${lun}_$which.bin") | Out-Null
    }
    Invoke-PhoneShell "rm -f /tmp/r8s_gpt.bin" | Out-Null
    $layout = Get-GptLayout ([IO.File]::ReadAllBytes((Join-Path $dest "gpt_${lun}_head.bin"))) $ss
    Write-Host ("GPT {0}: {1}" -f $lun, $(if ($layout) { "saved" } else { "no GPT (boot LUN), saved raw" }))
    $luns += [pscustomobject]@{ Name = $lun; SectorSize = $ss; Bytes = $bytes; SavedSectors = $n; HasGpt = [bool]$layout }
}

# ---- partitions ------------------------------------------------------------
$results = @()
$swAll = [Diagnostics.Stopwatch]::StartNew()
$i = 0
foreach ($p in $parts) {
    $i++
    if ($done.ContainsKey($p.Name) -and [int64]$done[$p.Name].Size -eq $p.Size) {
        Write-Host ("[{0}/{1}] {2}: already done" -f $i, $parts.Count, $p.Name)
        $results += $done[$p.Name]
        continue
    }
    $file = "$($p.Name).img.gz"
    $out = Join-Path $dest $file
    Write-Host ("[{0}/{1}] {2} ({3})" -f $i, $parts.Count, $p.Name, (Format-Size $p.Size))
    $cmd = "rm -f /tmp/r8s_f /tmp/r8s_sha /tmp/r8s_dd; mkfifo /tmp/r8s_f; " +
           "(sha256sum < /tmp/r8s_f > /tmp/r8s_sha) & " +
           "dd if=$($p.Node) bs=4194304 2>/tmp/r8s_dd | tee /tmp/r8s_f | gzip -1 -c; wait"
    $t = Receive-AdbStream $cmd $out "Backing up $($p.Name)"
    if ((Invoke-AdbRaw @("get-state")).Out -ne "recovery") {
        Remove-Item $out -ErrorAction SilentlyContinue
        throw ("The phone disconnected while saving $($p.Name). Nothing on the phone was changed. " +
               "Plug it back in, boot TWRP, and run .\Backup-Android.ps1 again: it continues from $($p.Name).")
    }

    $phoneSha = ((Invoke-PhoneShell "cat /tmp/r8s_sha") -split "\s+")[0]
    $ddLog = Invoke-PhoneShell "cat /tmp/r8s_dd"
    Invoke-PhoneShell "rm -f /tmp/r8s_f /tmp/r8s_sha /tmp/r8s_dd" | Out-Null
    if ($phoneSha -notmatch '^[0-9a-f]{64}$') { throw "$($p.Name): no hash from the phone. dd said: $ddLog" }
    if ($ddLog -match '(\d+) bytes' -and [int64]$Matches[1] -ne $p.Size) {
        throw "$($p.Name): read $($Matches[1]) of $($p.Size) bytes. dd said: $ddLog"
    }

    $gzBytes = (Get-Item $out).Length
    $verified = $false
    if (-not $SkipPcVerify) {
        $chk = Get-GzipSha256 $out "Checking $($p.Name)"
        if ($chk.Hash -ne $phoneSha -or $chk.Bytes -ne $p.Size) {
            throw "$($p.Name): the file on the PC doesn't match the phone (got $($chk.Bytes) bytes, $($chk.Hash)). Run the backup again."
        }
        $verified = $true
    }
    Write-Host ("    {0} -> {1} in {2:mm\:ss}{3}" -f (Format-Size $p.Size), (Format-Size $gzBytes), $t, $(if ($verified) { ", verified" } else { "" }))
    $results += [pscustomobject]@{ Name = $p.Name; Node = $p.Node; Size = $p.Size; Sha256 = $phoneSha; File = $file; GzBytes = $gzBytes }
    ConvertTo-Json -InputObject @($results) -Depth 3 | Set-Content (Join-Path $dest "progress.json")
}

$manifest = [pscustomobject]@{
    Format     = 1
    Created    = (Get-Date).ToString("s")
    Bootloader = $bl
    Device     = "r8s"
    Partitions = $results
    Luns       = $luns
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $dest "manifest.json")
Invoke-PhoneShell "mount /data 2>/dev/null; true" | Out-Null   # so TWRP shows the storage size again

$total = ($results | Measure-Object GzBytes -Sum).Sum
Write-Host ("`nBackup complete: {0}`n{1} partitions, {2} on disk, took {3:hh\:mm\:ss}." -f $dest, $results.Count, (Format-Size $total), $swAll.Elapsed)
Write-Host "Restore with: .\Restore-Android.ps1   (phone in TWRP)"
Write-Host "Keep a second copy of this folder somewhere else."
