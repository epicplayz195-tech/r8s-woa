<#
.SYNOPSIS
  Put the Windows built by tools\windows\Build-WindowsImage.ps1 on the phone.
  Phone in TWRP. Overwrites the "win" and "esp" partitions only.

.DESCRIPTION
  1. checks that the phone's win/esp are the ones the image was built for
     (same size, same GPT GUIDs: the boot configuration names them);
  2. writes win.sparse to "win" with r8s-sparse (discard, then only the blocks
     that hold data) and reads the whole partition back to check its digest;
  3. rewrites "esp": the UEFI Shell image with the self-test, plus Windows'
     boot files. Internal storage then runs: self-test -> Windows Boot
     Manager -> Windows. The log of the previous Windows run is saved to the
     esp on every start (Pull-Logs.ps1 fetches it) and the boot goes on, so
     Windows' own restarts during its first set-up are not interrupted.
     -Direct: no Shell / self-test, internal storage starts Windows directly
     (for every day, once it boots).

  The first start of Windows sets itself up (a reboot or two, "Getting
  ready"): give it 15-40 minutes on one CPU core, without touching it.
#>
param(
    [string]$In       = "D:\woa\out\windows",
    [string]$EspImage = "D:\woa\Mu-r8s-RELEASE\esp-r8s.img.gz",
    [string]$Scratch  = "D:\woa\scratch",
    [switch]$Direct,
    [switch]$NoVerify
)
. "$PSScriptRoot\Common.ps1"
. "$PSScriptRoot\AdbStream.ps1"

$manifest = Get-Content (Join-Path $In "manifest.json") -Raw | ConvertFrom-Json
$sparse = Join-Path $In $manifest.SparseFile
foreach ($p in $sparse, (Join-Path $In "esp\EFI\Microsoft\Boot\BCD"), $EspImage) {
    if (-not (Test-Path $p)) { throw "Not found: $p" }
}

$bl = Assert-R8sInRecovery
$win = Get-PartitionNode "win"
$null = Get-PartitionNode "esp"
$winBytes = [int64](Invoke-PhoneShell "blockdev --getsize64 $win")
if ($winBytes -ne [int64]$manifest.WinBytes) { throw "win is $winBytes bytes on the phone, the image is for $($manifest.WinBytes). Rebuild it." }

# The BCD names the partitions by GUID: they must be the same ones.
$lun = ($win -replace '^/dev/block/(sd[a-z])\d+$', '$1')
$tmp = Join-Path $env:TEMP "r8s-gpt-head.bin"
Invoke-PhoneShell "dd if=/dev/block/$lun of=/tmp/r8s_gpt.bin bs=4096 count=64 2>/dev/null" | Out-Null
Invoke-Adb pull /tmp/r8s_gpt.bin $tmp | Out-Null
Invoke-PhoneShell "rm -f /tmp/r8s_gpt.bin" | Out-Null
$g = [IO.File]::ReadAllBytes($tmp); Remove-Item $tmp
$ss = 4096
$diskGuid = [Guid]::new([byte[]]$g[($ss + 56)..($ss + 71)])
$entLba = [BitConverter]::ToUInt64($g, $ss + 72)
$entCount = [BitConverter]::ToUInt32($g, $ss + 80)
$entSize = [BitConverter]::ToUInt32($g, $ss + 84)
$guids = @{}
for ($i = 0; $i -lt $entCount; $i++) {
    $o = [int]($entLba * $ss + $i * $entSize)
    if ($o + 128 -gt $g.Length) { break }
    $name = [Text.Encoding]::Unicode.GetString($g, $o + 56, 72).TrimEnd([char]0)
    if ($name -eq "esp" -or $name -eq "win") { $guids[$name] = [Guid]::new([byte[]]$g[($o + 16)..($o + 31)]) }
}
if ($diskGuid.ToString() -ne $manifest.DiskGuid -or "$($guids['esp'])" -ne $manifest.EspGuid -or "$($guids['win'])" -ne $manifest.WinGuid) {
    throw "The phone's partition GUIDs differ from the ones the image was built for (re-partitioned?). Rebuild it with Build-WindowsImage.ps1."
}
Write-Host ("Image: {0}, {1:N1} GB of data, built {2}" -f $manifest.Edition, ($manifest.DataBytes / 1GB), $manifest.Created)
Confirm-Danger "This overwrites the phone's 'win' and 'esp' partitions (WinPE on the esp is replaced)."

# ---------------------------------------------------------------- win
$still = Dismount-PhoneFilesystems
Install-SparseHelper
Write-Host "==> Writing Windows to $win"
$t = Send-AdbStream "/tmp/r8s-sparse restore $win 2>/tmp/r8s_err" $sparse "Writing Windows"
$err = Invoke-PhoneShell "cat /tmp/r8s_err; rm -f /tmp/r8s_err"
if ($err -notmatch 'restored chunks \d+ digest ([0-9a-f]{16})' -or $Matches[1] -ne $manifest.SparseDigest) {
    throw "Writing win failed on the phone: $err"
}
Write-Host ("    written in {0:mm\:ss} ({1})" -f $t, ($err -replace "`n", "; "))
if (-not $NoVerify) {
    Write-Host "    reading it back..."
    $d = (Invoke-PhoneShell "/tmp/r8s-sparse digest $win") -split "\s+"
    if ($d[2] -ne $manifest.SparseDigest) { throw "Read-back digest $($d[2]) differs from the image ($($manifest.SparseDigest)). Run this again." }
    Write-Host "    verified"
}

# ---------------------------------------------------------------- esp
New-Item -ItemType Directory -Force -Path $Scratch | Out-Null
Write-Host "==> Rewriting the esp: Shell + self-test, then Windows' boot files"
$raw = Join-Path $Scratch "esp-r8s.img"
$inF = [IO.File]::OpenRead($EspImage)
$gz = New-Object IO.Compression.GZipStream($inF, [IO.Compression.CompressionMode]::Decompress)
$outF = [IO.File]::Create($raw)
try { $gz.CopyTo($outF) } finally { $outF.Dispose(); $gz.Dispose(); $inF.Dispose() }
Invoke-Adb push $raw /tmp/esp-r8s.img | Out-Null
Remove-Item $raw
Invoke-PhoneShell "umount /tmp/esp 2>/dev/null; dd if=/tmp/esp-r8s.img of=/dev/block/by-name/esp bs=4M; sync; rm -f /tmp/esp-r8s.img" | Out-Null

Invoke-PhoneShell "mkdir -p /tmp/esp; mount -t vfat /dev/block/by-name/esp /tmp/esp" | Out-Null
try {
    Invoke-PhoneShell "mv /tmp/esp/EFI/Boot/bootaa64.efi /tmp/shell.efi" | Out-Null
    foreach ($item in Get-ChildItem (Join-Path $In "esp") | Where-Object Name -ne "System Volume Information") {
        Write-Host "   $($item.Name)"
        Invoke-Adb push $item.FullName /tmp/esp/ | Out-Null
    }
    # Transaction logs left by the PC's bcdedit: stale next to the final BCD, the Boot Manager may replay them.
    Invoke-PhoneShell "rm -f /tmp/esp/EFI/Microsoft/Boot/BCD.LOG*; true" | Out-Null
    if ($Direct) {
        Invoke-PhoneShell "rm -f /tmp/shell.efi /tmp/esp/startup.nsh /tmp/esp/r8s-woa-esp.txt; rm -rf /tmp/esp/EFI/r8s; true" | Out-Null
    } else {
        Invoke-PhoneShell "mv /tmp/esp/EFI/Boot/bootaa64.efi /tmp/esp/EFI/Boot/winboot.efi && cp /tmp/shell.efi /tmp/esp/EFI/Boot/bootaa64.efi && rm /tmp/shell.efi" | Out-Null
        Invoke-PhoneShell "rm -f /tmp/esp/EFI/r8s/R8sTrace.efi /tmp/esp/r8s-noacpi /tmp/esp/r8s-winlog-new" | Out-Null
        $nsh = @'
@echo -off
echo " "
echo "=== r8s-woa: self-test, then Windows ==="
for %m in fs0 fs1 fs2 fs3 fs4
  if exist %m:\EFI\r8s\R8sDiag.efi then
    echo "Self-test (saved to %m:\r8s-diag.txt) ..."
    %m:\EFI\r8s\R8sDiag.efi
    if exist %m:\r8s-winlog-new then
      echo "Log of the last Windows run saved (Pull-Logs.ps1 in TWRP fetches it); going on."
      rm %m:\r8s-winlog-new
      stall 2000000
    endif
  endif
endfor
for %m in fs0 fs1 fs2 fs3 fs4
  if exist %m:\EFI\Boot\winboot.efi then
    %m:
    echo "Starting Windows ..."
    \EFI\Boot\winboot.efi
    echo " "
    echo "Boot Manager returned, lasterror = %lasterror%"
    goto Done
  endif
endfor
echo "winboot.efi not found on fs0-fs4"
:Done
'@
        $nshPath = Join-Path $Scratch "startup.nsh"
        [IO.File]::WriteAllText($nshPath, ($nsh -replace "`r?`n", "`r`n"), [Text.Encoding]::ASCII)
        Invoke-Adb push $nshPath /tmp/esp/startup.nsh | Out-Null
    }
    Write-Host (Invoke-PhoneShell "ls -l /tmp/esp/ /tmp/esp/EFI/Boot/ /tmp/esp/EFI/Microsoft/Boot/ 2>&1 | head -40; true")
} finally {
    Invoke-PhoneShell "sync; umount /tmp/esp" | Out-Null
}

Write-Host "`nDone. Reboot -> System, choose internal storage." -ForegroundColor Green
Write-Host "Windows sets itself up on the first start (15-40 min, a reboot or two): leave it alone."
Write-Host "If it stops at the Silicium menu after one of its restarts, choose internal storage again."
Write-Host "It signs in as '$($manifest.UserName)' by itself."
