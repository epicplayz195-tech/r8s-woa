<#
.SYNOPSIS
  Replace Android's userdata with an ESP and a Windows partition, then
  write the test ESP image. Phone must be in TWRP.

.DESCRIPTION
  TWRP's sgdisk is Android's cut-down build (no -p/-n/-d), so this script
  edits the GPT itself:

    1. pulls the primary and backup GPT of userdata's LUN,
    2. checks every CRC and that the two copies agree,
    3. turns 'userdata' into 'esp' (EFI System, EspSizeMiB) and adds 'win'
       (Basic data, the rest of userdata's space) in the first free slot,
    4. recomputes the CRCs, writes both GPT copies and reads them back,
    5. writes esp-r8s.img.gz (FAT32 + UEFI Shell self-test) to the ESP.

  Nothing outside userdata's sectors changes. -DryRun stops after step 3 and
  saves the new tables to D:\woa\scratch for inspection.
  Destroys all Android data. Run Backup-Phone.ps1 first.
#>
param(
    [int]$EspSizeMiB = 1024,
    [string]$EspImage = "D:\woa\out\esp-r8s.img.gz",
    [string]$BackupDir = "D:\woa\backups",
    [string]$Scratch = "D:\woa\scratch",
    [switch]$DryRun
)
. "$PSScriptRoot\Common.ps1"

# ---------------------------------------------------------------- helpers --
$script:CrcTable = [uint64[]]::new(256)
for ($n = 0; $n -lt 256; $n++) {
    [uint64]$c = $n
    for ($k = 0; $k -lt 8; $k++) {
        if ($c -band 1) { $c = [uint64]3988292384 -bxor ($c -shr 1) } else { $c = $c -shr 1 }
    }
    $script:CrcTable[$n] = $c
}
function Get-Crc32([byte[]]$Data, [int]$Offset, [int]$Count) {
    [uint64]$crc = 4294967295
    for ($i = $Offset; $i -lt $Offset + $Count; $i++) {
        $crc = $script:CrcTable[[int](($crc -bxor $Data[$i]) -band 255)] -bxor ($crc -shr 8)
    }
    return [uint32]($crc -bxor 4294967295)
}
function Get-U32([byte[]]$b, [int]$o) { [BitConverter]::ToUInt32($b, $o) }
function Get-U64([byte[]]$b, [int]$o) { [BitConverter]::ToUInt64($b, $o) }
function Set-Bytes([byte[]]$Dst, [int]$Off, [byte[]]$Src) { [Array]::Copy($Src, 0, $Dst, $Off, $Src.Length) }
function Get-Slice([byte[]]$b, [int]$o, [int]$n) { $r = [byte[]]::new($n); [Array]::Copy($b, $o, $r, 0, $n); return ,$r }

function Read-Header([byte[]]$Block) {
    $hsz = [int](Get-U32 $Block 12)
    $tmp = Get-Slice $Block 0 $hsz
    Set-Bytes $tmp 16 ([byte[]]::new(4))
    [pscustomobject]@{
        Sig       = [Text.Encoding]::ASCII.GetString($Block, 0, 8)
        Size      = $hsz
        CrcOk     = ((Get-Crc32 $tmp 0 $hsz) -eq (Get-U32 $Block 16))
        MyLba     = Get-U64 $Block 24
        AltLba    = Get-U64 $Block 32
        EntLba    = Get-U64 $Block 72
        EntCount  = [int](Get-U32 $Block 80)
        EntSize   = [int](Get-U32 $Block 84)
        EntCrc    = Get-U32 $Block 88
    }
}
function Update-HeaderCrcs([byte[]]$Block, [int]$HeaderSize, [uint32]$EntriesCrc) {
    Set-Bytes $Block 88 ([BitConverter]::GetBytes($EntriesCrc))
    Set-Bytes $Block 16 ([byte[]]::new(4))
    Set-Bytes $Block 16 ([BitConverter]::GetBytes((Get-Crc32 $Block 0 $HeaderSize)))
}
function Get-EntryName([byte[]]$Ent, [int]$Off) {
    [Text.Encoding]::Unicode.GetString($Ent, $Off + 56, 72).TrimEnd([char]0)
}
function Set-Entry([byte[]]$Ent, [int]$Off, [Guid]$Type, [Guid]$Unique, [uint64]$First, [uint64]$Last, [string]$Name) {
    $e = [byte[]]::new(128)
    Set-Bytes $e 0  $Type.ToByteArray()          # GPT stores GUIDs mixed-endian, as .NET does
    Set-Bytes $e 16 $Unique.ToByteArray()
    Set-Bytes $e 32 ([BitConverter]::GetBytes($First))
    Set-Bytes $e 40 ([BitConverter]::GetBytes($Last))
    Set-Bytes $e 56 ([Text.Encoding]::Unicode.GetBytes($Name))
    [Array]::Copy($e, 0, $Ent, $Off, 128)
}
function Pull-Sectors([string]$Disk, [uint64]$Lba, [int]$Count, [int]$Ss, [string]$Local) {
    Invoke-PhoneShell "dd if=$Disk of=/tmp/r8s_gpt.bin bs=$Ss skip=$Lba count=$Count" | Out-Null
    Invoke-Adb pull /tmp/r8s_gpt.bin $Local | Out-Null
    Invoke-PhoneShell "rm -f /tmp/r8s_gpt.bin" | Out-Null
    return ,[System.IO.File]::ReadAllBytes($Local)
}
function Push-Sectors([string]$Disk, [uint64]$Lba, [byte[]]$Data, [int]$Ss, [string]$Local) {
    [System.IO.File]::WriteAllBytes($Local, $Data)
    $count = [int]($Data.Length / $Ss)
    Invoke-Adb push $Local /tmp/r8s_gpt.bin | Out-Null
    Invoke-PhoneShell "dd if=/tmp/r8s_gpt.bin of=$Disk bs=$Ss seek=$Lba count=$count conv=notrunc; sync" | Out-Null
    Invoke-PhoneShell "rm -f /tmp/r8s_gpt.bin" | Out-Null
    $want = (Get-FileHash $Local -Algorithm SHA256).Hash.ToLower()
    $got  = Invoke-PhoneShell "dd if=$Disk bs=$Ss skip=$Lba count=$count 2>/dev/null | sha256sum | cut -d' ' -f1"
    if ($got -ne $want) { throw "Write verification failed at LBA $Lba." }
}

# ----------------------------------------------------------------- checks --
$null = Assert-R8sInRecovery
if (-not (Get-ChildItem -Path $BackupDir -Recurse -Filter "SHA256SUMS" -ErrorAction SilentlyContinue)) {
    throw "No backup found under $BackupDir. Run Backup-Phone.ps1 first."
}
if (-not (Test-Path $EspImage) -and (Test-Path "D:\woa\Mu-r8s-RELEASE\esp-r8s.img.gz")) {
    $EspImage = "D:\woa\Mu-r8s-RELEASE\esp-r8s.img.gz"
}
if (-not (Test-Path $EspImage)) { throw "ESP image not found: $EspImage (it comes with the GitHub Actions build)." }
if ($EspSizeMiB -lt 1024) { throw "EspSizeMiB must be at least 1024 (the ESP image is 1 GiB)." }
New-Item -ItemType Directory -Force -Path $Scratch | Out-Null

$node = Get-PartitionNode "userdata"           # e.g. /dev/block/sda32
if ($node -notmatch "^/dev/block/(sd[a-z])(\d+)$") { throw "Unexpected userdata node: $node" }
$disk = "/dev/block/$($Matches[1])"
$num  = [int]$Matches[2]
$ss   = [int](Invoke-PhoneShell "blockdev --getss $disk")
if ($ss -ne 4096 -and $ss -ne 512) { throw "Unexpected sector size $ss." }
Write-Host "userdata is $disk partition $num ($ss-byte sectors)"

# ------------------------------------------------------ 1-2. read + check --
$pBlocks = Pull-Sectors $disk 0 64 $ss (Join-Path $Scratch "gpt_primary_area.bin")
$pHdrBlock = Get-Slice $pBlocks $ss $ss
$ph = Read-Header $pHdrBlock
if ($ph.Sig -ne "EFI PART" -or -not $ph.CrcOk -or $ph.MyLba -ne 1) { throw "Primary GPT header is not valid." }
$entBytesLen = $ph.EntCount * $ph.EntSize
$entBlocks   = [int][math]::Ceiling($entBytesLen / $ss)
if ($ph.EntSize -ne 128) { throw "Unexpected GPT entry size $($ph.EntSize)." }
if (($ph.EntLba + $entBlocks) -gt 64) { throw "Primary entries are outside the area read." }
$entries = Get-Slice $pBlocks ([int]($ph.EntLba * $ss)) ($entBlocks * $ss)
if ((Get-Crc32 $entries 0 $entBytesLen) -ne $ph.EntCrc) { throw "Primary GPT entries CRC mismatch." }

$bArea = Pull-Sectors $disk ($ph.AltLba - $entBlocks) ($entBlocks + 1) $ss (Join-Path $Scratch "gpt_backup_area.bin")
$bHdrBlock = Get-Slice $bArea ($entBlocks * $ss) $ss
$bh = Read-Header $bHdrBlock
if ($bh.Sig -ne "EFI PART" -or -not $bh.CrcOk -or $bh.MyLba -ne $ph.AltLba) { throw "Backup GPT header is not valid." }
if ($bh.EntLba -ne ($ph.AltLba - $entBlocks)) { throw "Backup entries are not where expected (LBA $($bh.EntLba))." }
$bEntries = Get-Slice $bArea 0 ($entBlocks * $ss)
if ((Get-Crc32 $bEntries 0 $entBytesLen) -ne $bh.EntCrc) { throw "Backup GPT entries CRC mismatch." }
if ([Convert]::ToBase64String($bEntries, 0, $entBytesLen) -ne [Convert]::ToBase64String($entries, 0, $entBytesLen)) {
    throw "Primary and backup GPT entries differ. Not touching this disk."
}
Write-Host "GPT OK: both copies valid and identical, $($ph.EntCount) slots, backup at LBA $($ph.AltLba)"

# --------------------------------------------------------- 3. new layout --
$ui = $num - 1; $uo = $ui * 128
$names = for ($i = 0; $i -lt $ph.EntCount; $i++) { Get-EntryName $entries ($i * 128) }
if ($names[$ui] -ne "userdata") { throw "Slot $num is '$($names[$ui])', not userdata." }
if ($names -contains "esp" -or $names -contains "win") { throw "esp/win already exist. Already partitioned?" }
$free = -1
for ($i = 0; $i -lt $ph.EntCount; $i++) {
    if ((Get-U64 $entries ($i * 128 + 32)) -eq 0 -and (Get-U64 $entries ($i * 128 + 40)) -eq 0) { $free = $i; break }
}
if ($free -lt 0) { throw "No free GPT slot for 'win'." }

$first   = Get-U64 $entries ($uo + 32)
$last    = Get-U64 $entries ($uo + 40)
$uGuid   = [Guid]::new([byte[]](Get-Slice $entries ($uo + 16) 16))
$espLast = $first + [uint64]([int64]$EspSizeMiB * 1MB / $ss) - 1
if ($espLast + [uint64](8GB / $ss) -gt $last) { throw "userdata is too small for a $EspSizeMiB MiB ESP plus 8 GB for Windows." }

$espType = [Guid]"C12A7328-F81F-11D2-BA4B-00A0C93EC93B"   # EFI System
$winType = [Guid]"EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"   # Microsoft basic data
$newEntries = Get-Slice $entries 0 $entries.Length
Set-Entry $newEntries $uo           $espType $uGuid           $first         $espLast "esp"
Set-Entry $newEntries ($free * 128) $winType ([Guid]::NewGuid()) ($espLast + 1) $last  "win"
$crc = Get-Crc32 $newEntries 0 $entBytesLen
Update-HeaderCrcs $pHdrBlock $ph.Size $crc
Update-HeaderCrcs $bHdrBlock $bh.Size $crc
if (-not (Read-Header $pHdrBlock).CrcOk -or -not (Read-Header $bHdrBlock).CrcOk) { throw "Internal error: header CRC." }

$fmt = "{0,-9} slot {1,-3} sectors {2,10}-{3,-10} {4,9:N0} MB"
Write-Host "`nPlan on ${disk}:"
Write-Host ($fmt -f "userdata", $num, $first, $last, (($last - $first + 1) * $ss / 1MB)) " (now)"
Write-Host ($fmt -f "esp", $num, $first, $espLast, (($espLast - $first + 1) * $ss / 1MB))
Write-Host ($fmt -f "win", ($free + 1), ($espLast + 1), $last, (($last - $espLast) * $ss / 1MB))

[System.IO.File]::WriteAllBytes((Join-Path $Scratch "gpt_new_entries.bin"), $newEntries)
[System.IO.File]::WriteAllBytes((Join-Path $Scratch "gpt_new_primary_header.bin"), $pHdrBlock)
[System.IO.File]::WriteAllBytes((Join-Path $Scratch "gpt_new_backup_header.bin"), $bHdrBlock)
if ($DryRun) { Write-Host "`nDry run: nothing written. New tables saved in $Scratch."; return }

Confirm-Danger "This deletes userdata (all Android data) and rewrites the partition table of $disk."

# ------------------------------------------------------- 4. write GPT -----
Invoke-PhoneShell "umount /data 2>/dev/null; umount /sdcard 2>/dev/null; true" | Out-Null
Write-Host "==> Writing primary GPT"
Push-Sectors $disk $ph.EntLba  $newEntries $ss (Join-Path $Scratch "w_pent.bin")
Push-Sectors $disk 1           $pHdrBlock  $ss (Join-Path $Scratch "w_phdr.bin")
Write-Host "==> Writing backup GPT"
Push-Sectors $disk $bh.EntLba  $newEntries $ss (Join-Path $Scratch "w_bent.bin")
Push-Sectors $disk $ph.AltLba  $bHdrBlock  $ss (Join-Path $Scratch "w_bhdr.bin")
Write-Host "GPT written and verified."

# ------------------------------------------------------- 5. ESP image -----
# The kernel still has the old (larger) userdata at the same start sector,
# so write through ${disk}${num}; by-name links update after a reboot.
Write-Host "==> Writing the ESP image (FAT32 + UEFI Shell)"
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

Write-Host "`nDone. Partition table and ESP written and verified."
Write-Host "Reboot TWRP once so 'esp' and 'win' appear."
Write-Host "Then boot the UEFI and pick internal storage: the UEFI Shell should start from UFS."
