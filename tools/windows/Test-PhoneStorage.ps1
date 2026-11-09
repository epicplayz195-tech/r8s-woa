<#
.SYNOPSIS
  Read-only check that the phone's UFS works through the Silicium UEFI.
  Run as Administrator while the phone is in UEFI "Mass Storage" mode.

.DESCRIPTION
  Never writes to the phone. It:
    1. finds the phone's USB disk (refuses the PC's own system/boot disks),
    2. reads and parses the primary GPT at the start of the disk,
    3. reads the backup GPT at the very END of the disk (proves reads work
       across the whole LUN, not just the first blocks),
    4. checks the two GPT headers agree,
    5. reads 16 MiB at the start, middle and end twice each and compares
       hashes (proves reads are stable, not returning garbage),
    6. lists every partition by name.

  IMPORTANT: when the phone connects, Windows may ask to "Initialize disk"
  or "Format disk". Always click CANCEL.

.EXAMPLE
  .\Test-PhoneStorage.ps1
  .\Test-PhoneStorage.ps1 -DiskNumber 3
#>
param([int]$DiskNumber = -1)
$ErrorActionPreference = "Stop"

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this from an elevated PowerShell (raw disk reads need admin)."
}

# ---- 1. pick the disk -------------------------------------------------------
$candidates = Get-Disk | Where-Object { $_.BusType -eq "USB" -and -not $_.IsSystem -and -not $_.IsBoot }
if ($DiskNumber -ge 0) {
    $disk = Get-Disk -Number $DiskNumber
    if ($disk.IsSystem -or $disk.IsBoot) { throw "Disk $DiskNumber is this PC's system disk. Refusing." }
} else {
    if (-not $candidates) { throw "No USB disk found. Is the phone in UEFI Mass Storage mode and plugged in?" }
    $candidates | Format-Table Number, FriendlyName, @{n="SizeGB";e={[math]::Round($_.Size/1GB,1)}}, LogicalSectorSize, PartitionStyle -AutoSize | Out-Host
    if (@($candidates).Count -gt 1) { throw "Several USB disks found; rerun with -DiskNumber <n>." }
    $disk = $candidates
}
$ss = [int]$disk.LogicalSectorSize
Write-Host ("Testing disk {0}: {1}, {2:N1} GB, {3}-byte sectors" -f $disk.Number, $disk.FriendlyName, ($disk.Size/1GB), $ss)

$stream = [System.IO.FileStream]::new("\\.\PhysicalDrive$($disk.Number)",
    [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)

function Read-Bytes([int64]$Offset, [int]$Count) {
    $buf = New-Object byte[] $Count
    $stream.Position = $Offset
    $got = 0
    while ($got -lt $Count) {
        $n = $stream.Read($buf, $got, $Count - $got)
        if ($n -le 0) { throw "Short read at offset $Offset (got $got of $Count bytes)." }
        $got += $n
    }
    return ,$buf
}
function U32($b, $o) { [BitConverter]::ToUInt32($b, $o) }
function U64($b, $o) { [BitConverter]::ToUInt64($b, $o) }
function Parse-Header([byte[]]$b) {
    $sig = [Text.Encoding]::ASCII.GetString($b, 0, 8)
    [pscustomobject]@{
        Signature  = $sig
        MyLba      = U64 $b 24
        AltLba     = U64 $b 32
        FirstUsable= U64 $b 40
        LastUsable = U64 $b 48
        DiskGuid   = [Guid]::new([byte[]]$b[56..71])
        EntriesLba = U64 $b 72
        EntryCount = U32 $b 80
        EntrySize  = U32 $b 84
        EntriesCrc = U32 $b 88
    }
}

$fail = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "  [OK]   $what" -ForegroundColor Green }
    else     { Write-Host "  [FAIL] $what" -ForegroundColor Red; $script:fail++ }
}

try {
    $totalSectors = [int64]($disk.Size / $ss)

    # ---- 2. primary GPT -----------------------------------------------------
    Write-Host "`nPrimary GPT"
    $primary = Parse-Header (Read-Bytes ([int64]$ss) $ss)
    Check ($primary.Signature -eq "EFI PART") "header signature at LBA 1 ('$($primary.Signature)')"
    if ($primary.Signature -ne "EFI PART") { throw "No GPT at LBA 1; reads from the start of the disk are broken (or this isn't the phone)." }

    # ---- 3. backup GPT at the end -------------------------------------------
    Write-Host "`nBackup GPT (end of disk, LBA $($primary.AltLba) of $($totalSectors - 1))"
    $backup = Parse-Header (Read-Bytes ([int64]$primary.AltLba * $ss) $ss)
    Check ($backup.Signature -eq "EFI PART") "backup header readable at the last LBA"
    # ---- 4. agree ------------------------------------------------------------
    Check ($backup.DiskGuid -eq $primary.DiskGuid) "disk GUIDs match ($($primary.DiskGuid))"
    Check ($backup.EntriesCrc -eq $primary.EntriesCrc) "partition-entry CRCs match"
    Check ($backup.AltLba -eq 1) "backup header points back to LBA 1"

    # ---- 5. stable reads -----------------------------------------------------
    Write-Host "`nRepeat-read test (16 MiB at start / middle / end, read twice each)"
    $chunk = 16MB
    $lastByte = [int64]$primary.LastUsable * $ss
    $firstByte = [int64]$primary.FirstUsable * $ss
    $mid = [int64](($firstByte + $lastByte) / 2 / $ss) * $ss
    $sha = [Security.Cryptography.SHA256]::Create()
    foreach ($pt in @(@("start", 0), @("middle", $mid), @("end", ($lastByte - $chunk)))) {
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $h1 = [BitConverter]::ToString($sha.ComputeHash((Read-Bytes $pt[1] $chunk)))
        $mbps = [math]::Round(16 / [math]::Max($sw.Elapsed.TotalSeconds, 0.001), 1)
        $h2 = [BitConverter]::ToString($sha.ComputeHash((Read-Bytes $pt[1] $chunk)))
        Check ($h1 -eq $h2) ("{0,-6} at {1:N0} MiB, identical twice, {2} MB/s" -f $pt[0], ($pt[1]/1MB), $mbps)
    }

    # ---- 6. partitions -------------------------------------------------------
    Write-Host "`nPartitions"
    $entBytes = Read-Bytes ([int64]$primary.EntriesLba * $ss) ([int]($primary.EntryCount * $primary.EntrySize))
    $rows = for ($i = 0; $i -lt $primary.EntryCount; $i++) {
        $o = $i * $primary.EntrySize
        $first = U64 $entBytes ($o + 32); $last = U64 $entBytes ($o + 40)
        if ($first -eq 0 -and $last -eq 0) { continue }
        [pscustomobject]@{
            No     = $i + 1
            Name   = [Text.Encoding]::Unicode.GetString($entBytes, $o + 56, 72).TrimEnd([char]0)
            SizeMB = [math]::Round(($last - $first + 1) * $ss / 1MB, 1)
            First  = $first
            Last   = $last
        }
    }
    $rows | Format-Table -AutoSize | Out-Host
    Check (@($rows).Count -gt 0) "$(@($rows).Count) partitions listed"
}
finally { $stream.Dispose() }

Write-Host ""
if ($fail -eq 0) {
    Write-Host "UFS works through the UEFI: whole-disk reads are correct and stable." -ForegroundColor Green
} else {
    Write-Host "$fail check(s) failed. Send this output." -ForegroundColor Red
    exit 1
}
