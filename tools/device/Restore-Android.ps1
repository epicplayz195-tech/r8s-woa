<#
.SYNOPSIS
  Put an Android backup made by Backup-Android.ps1 back on the phone, exactly
  as it was (ROM, root, modules, apps, data). Phone in TWRP.

.DESCRIPTION
  Run it, and if it says so, reboot TWRP and run it again:

  Pass 1: if the partition table differs from the backup (for example
          userdata was split into esp + win for Windows), it writes the saved
          table back and asks you to reboot TWRP. This deletes esp and win.
  Pass 2: streams every image back, decompressing on the phone, then reads
          each partition back and checks its SHA-256 against the backup.

  By default it restores the Android partitions only (boot, recovery, dtbo,
  vbmeta*, super, userdata, metadata, cache, uh, misc, prism, optics, ...).
  It never writes efs / sec_efs / cpefs (IMEI) unless you name them with
  -Partitions. -All restores everything else in the backup.

  The phone must be the one the backup came from, on the same bootloader.
#>
param(
    [string]$BackupDir,
    [string[]]$Partitions,
    [switch]$All,
    [switch]$NoVerify
)
. "$PSScriptRoot\Common.ps1"
. "$PSScriptRoot\AdbStream.ps1"

$AndroidSet = @("boot", "init_boot", "vendor_boot", "dtbo", "vbmeta", "vbmeta_samsung", "vbmeta_system",
                "recovery", "super", "userdata", "metadata", "cache", "uh", "misc", "prism", "optics",
                "omr", "persist", "spu", "keydata", "keyrefuge", "steady")
$NeverByDefault = @("efs", "sec_efs", "cpefs")

if (-not $BackupDir) {
    $BackupDir = Get-ChildItem "D:\woa\backups" -Directory -Filter "android-*" -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName "manifest.json") } |
        Sort-Object Name | Select-Object -Last 1 -ExpandProperty FullName
    if (-not $BackupDir) { throw "No Android backup found in D:\woa\backups. Pass -BackupDir." }
}
$manifest = Get-Content (Join-Path $BackupDir "manifest.json") -Raw | ConvertFrom-Json
Write-Host "Backup: $BackupDir (made $($manifest.Created), bootloader $($manifest.Bootloader))"

$bl = Assert-R8sInRecovery
if ($bl -ne $manifest.Bootloader) {
    Confirm-Danger ("The phone's bootloader is $bl, but the backup was made on $($manifest.Bootloader). " +
        "The ROM may not boot on a different bootloader, and an older bootloader can't be flashed back.")
}
$missing = Invoke-PhoneShell "for t in gzip sha256sum dd blockdev; do command -v `$t >/dev/null 2>&1 || echo `$t; done"
if ($missing) { throw "TWRP is missing: $($missing -replace "`n", ', ')." }

# ---- check adb exec-in works before trusting it with GBs --------------------
$probe = Join-Path $env:TEMP "r8s_probe.bin"
$rnd = New-Object byte[] (1MB); (New-Object Random).NextBytes($rnd); [IO.File]::WriteAllBytes($probe, $rnd)
$null = Send-AdbStream "cat > /tmp/r8s_probe" $probe "adb self-test"
$want = (Get-FileHash $probe -Algorithm SHA256).Hash.ToLower()
$got = ((Invoke-PhoneShell "sha256sum /tmp/r8s_probe; rm -f /tmp/r8s_probe") -split "\s+")[0]
Remove-Item $probe
if ($got -ne $want) { throw "adb exec-in self-test failed (sent $want, phone got $got). Update platform-tools and retry." }

# ---- pass 1: partition tables ----------------------------------------------
function Get-PhoneSectors([string]$Lun, [int64]$Lba, [int]$Count, [int]$Ss) {
    $local = Join-Path $env:TEMP "r8s_cur.bin"
    Invoke-PhoneShell "dd if=/dev/block/$Lun of=/tmp/r8s_cur.bin bs=$Ss skip=$Lba count=$Count 2>/dev/null" | Out-Null
    Invoke-Adb pull /tmp/r8s_cur.bin $local | Out-Null
    Invoke-PhoneShell "rm -f /tmp/r8s_cur.bin" | Out-Null
    $b = [IO.File]::ReadAllBytes($local); Remove-Item $local
    return ,$b
}
function Write-PhoneSectors([string]$Lun, [int64]$Lba, [byte[]]$Data, [int]$Ss) {
    $local = Join-Path $env:TEMP "r8s_new.bin"
    [IO.File]::WriteAllBytes($local, $Data)
    $count = [int]($Data.Length / $Ss)
    Invoke-Adb push $local /tmp/r8s_new.bin | Out-Null
    Invoke-PhoneShell "dd if=/tmp/r8s_new.bin of=/dev/block/$Lun bs=$Ss seek=$Lba count=$count conv=notrunc 2>/dev/null; sync; rm -f /tmp/r8s_new.bin" | Out-Null
    $w = (Get-FileHash $local -Algorithm SHA256).Hash.ToLower(); Remove-Item $local
    $r = Invoke-PhoneShell "dd if=/dev/block/$Lun bs=$Ss skip=$Lba count=$count 2>/dev/null | sha256sum | cut -d' ' -f1"
    if ($r -ne $w) { throw "Verify failed writing $Lun at LBA $Lba." }
}
function Get-Range([byte[]]$b, [int]$o, [int]$n) { $r = New-Object byte[] $n; [Array]::Copy($b, $o, $r, 0, $n); return ,$r }
function Test-Same([byte[]]$a, [byte[]]$b) { [Convert]::ToBase64String($a) -eq [Convert]::ToBase64String($b) }

$gptFixes = @()
foreach ($l in @($manifest.Luns | Where-Object HasGpt)) {
    $ss = [int]$l.SectorSize
    $nowBytes = [int64](Invoke-PhoneShell "blockdev --getsize64 /dev/block/$($l.Name) 2>/dev/null || echo 0")
    if ($nowBytes -ne [int64]$l.Bytes) { throw "LUN $($l.Name) is $nowBytes bytes now, $($l.Bytes) in the backup. Wrong phone?" }
    $head = [IO.File]::ReadAllBytes((Join-Path $BackupDir "gpt_$($l.Name)_head.bin"))
    $tail = [IO.File]::ReadAllBytes((Join-Path $BackupDir "gpt_$($l.Name)_tail.bin"))
    $g = Get-GptLayout $head $ss
    $sectors = [int64]($l.Bytes / $ss)
    if ($g.AltLba -ne ($sectors - 1)) { throw "LUN $($l.Name): backup GPT header isn't at the last sector; not restoring it automatically." }
    $pWant = Get-Range $head 0 ($g.PrimarySectors * $ss)
    $bWant = Get-Range $tail ($tail.Length - $g.BackupSectors * $ss) ($g.BackupSectors * $ss)
    $bLba = $sectors - $g.BackupSectors
    $pNow = Get-PhoneSectors $l.Name 0 $g.PrimarySectors $ss
    $bNow = Get-PhoneSectors $l.Name $bLba $g.BackupSectors $ss
    if (-not (Test-Same $pNow $pWant) -or -not (Test-Same $bNow $bWant)) {
        $gptFixes += [pscustomobject]@{ Lun = $l.Name; Ss = $ss; P = $pWant; B = $bWant; BLba = $bLba }
    }
}
if ($gptFixes.Count -gt 0) {
    Write-Host "`nThe partition table differs from the backup on: $(($gptFixes | ForEach-Object Lun) -join ', ')"
    Write-Host "(Expected if the phone was partitioned for Windows: esp and win will be removed.)"
    Confirm-Danger "Write the backed-up partition table back? Everything on esp/win is lost."
    $null = Dismount-PhoneFilesystems
    foreach ($f in $gptFixes) {
        Write-Host "==> GPT $($f.Lun)"
        Write-PhoneSectors $f.Lun 0 $f.P $f.Ss
        Write-PhoneSectors $f.Lun $f.BLba $f.B $f.Ss
    }
    Write-Host "`nPartition table restored and verified."
    Write-Host "Now reboot TWRP (Reboot > Recovery) so it sees the old partitions, then run .\Restore-Android.ps1 again." -ForegroundColor Yellow
    return
}
Write-Host "Partition table matches the backup."

# ---- pass 2: images --------------------------------------------------------
$inBackup = @($manifest.Partitions)
if ($Partitions) {
    $todo = @($inBackup | Where-Object { $Partitions -contains $_.Name })
    $unknown = @($Partitions | Where-Object { $inBackup.Name -notcontains $_ })
    if ($unknown) { throw "Not in the backup: $($unknown -join ', ')" }
} elseif ($All) {
    $todo = @($inBackup | Where-Object { $NeverByDefault -notcontains $_.Name })
} else {
    $todo = @($inBackup | Where-Object { $AndroidSet -contains $_.Name })
}
# Biggest last, so a problem with the small ones shows up quickly.
$todo = @($todo | Sort-Object { [int64]$_.Size })

$now = @{}
Get-PhonePartitions | ForEach-Object { $now[$_.Name] = $_ }
foreach ($p in $todo) {
    if (-not $now.ContainsKey($p.Name)) { throw "Partition '$($p.Name)' doesn't exist on the phone now. Reboot TWRP and retry." }
    if ($now[$p.Name].Size -ne [int64]$p.Size) { throw "'$($p.Name)' is $($now[$p.Name].Size) bytes now but $($p.Size) in the backup." }
    if (-not (Test-Path (Join-Path $BackupDir $p.File))) { throw "Missing file $($p.File) in the backup." }
}

Write-Host "`nWill restore $($todo.Count) partitions:"
$todo | ForEach-Object { Write-Host ("  {0,-16} {1}" -f $_.Name, (Format-Size ([int64]$_.Size))) }
Confirm-Danger "This overwrites those partitions on the phone (Windows or anything else on them is lost)."

Install-SparseHelper
$still = Dismount-PhoneFilesystems
if ($still) { throw "TWRP still has $($still -join ', ') mounted and won't let go. Reboot TWRP, don't enter a PIN or decrypt, and run this again." }
$swAll = [Diagnostics.Stopwatch]::StartNew()
$i = 0
foreach ($p in $todo) {
    $i++
    $node = $now[$p.Name].Node
    Write-Host ("[{0}/{1}] {2} -> {3}" -f $i, $todo.Count, $p.Name, $node)
    if ($p.Format -eq "sparse") {
        $t = Send-AdbStream "/tmp/r8s-sparse restore $node 2>/tmp/r8s_err" (Join-Path $BackupDir $p.File) "Restoring $($p.Name)"
        $err = Invoke-PhoneShell "cat /tmp/r8s_err; rm -f /tmp/r8s_err"
        if ($err -notmatch 'restored chunks \d+ digest ([0-9a-f]{16})' -or $Matches[1] -ne $p.Digest) {
            throw "$($p.Name): restore failed on the phone: $err"
        }
        if (-not $NoVerify) {
            Write-Host "    checking..."
            $d = (Invoke-PhoneShell "/tmp/r8s-sparse digest $node") -split "\s+"
            if ($d[2] -ne $p.Digest) { throw "$($p.Name): read-back digest $($d[2]) doesn't match the backup $($p.Digest). Run the restore again." }
        }
    } else {
        $t = Send-AdbStream "gzip -dc | dd of=$node bs=4194304 2>/tmp/r8s_dd; sync" (Join-Path $BackupDir $p.File) "Restoring $($p.Name)"
        $ddLog = Invoke-PhoneShell "cat /tmp/r8s_dd; rm -f /tmp/r8s_dd"
        if ($ddLog -match '(\d+) bytes' -and [int64]$Matches[1] -ne [int64]$p.Size) {
            throw "$($p.Name): wrote $($Matches[1]) of $($p.Size) bytes. dd said: $ddLog"
        }
        if (-not $NoVerify) {
            Write-Host "    checking..."
            $sha = ((Invoke-PhoneShell "sha256sum $node") -split "\s+")[0]
            if ($sha -ne $p.Sha256) { throw "$($p.Name): read-back hash $sha doesn't match the backup $($p.Sha256). Run the restore again." }
        }
    }
    Write-Host ("    done in {0:mm\:ss}{1}" -f $t, $(if ($NoVerify) { "" } else { ", verified" }))
}

Write-Host ("`nAndroid restored in {0:hh\:mm\:ss}. Reboot > System." -f $swAll.Elapsed) -ForegroundColor Green
