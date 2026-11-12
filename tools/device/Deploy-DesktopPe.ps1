<#
.SYNOPSIS
  Deploy the desktop WinPE (its own boot.sdi/BCD/bootaa64.efi, ARM64) to the
  phone's ESP and boot it:  UEFI Shell -> R8sDiag self-test -> Boot Manager
  -> desktop.  No call tracer: that one is tuned to the 22621 bootmgr and
  would misbehave with this build-19041 one.  Phone must be in TWRP.

  No drivers are injected into this PE (DISM /Add-Driver fails with error 216
  on this older image from an x64 host).  Load drivers at runtime instead,
  from the SD card:  pnputil /add-driver X:\r8slog.inf /install
#>
param(
    [string]$Source   = "D:\woa\other-pe\winpe_desktop_nodrivers",
    [string]$EspImage = "D:\woa\Mu-r8s-RELEASE\esp-r8s.img.gz",
    [string]$Scratch  = "D:\woa\scratch",
    [switch]$BcdOnly,   # just re-push efi\microsoft\boot\bcd (fast; after a bcdedit tweak)
    [switch]$Trace,     # fast: restore the call tracer + a tracing startup.nsh (no 600 MB re-push)
    [string]$TraceEfi = "D:\woa\Mu-r8s-RELEASE\R8sTrace.efi"
)
. "$PSScriptRoot\Common.ps1"

$null = Assert-R8sInRecovery
foreach ($p in $Source, $EspImage) { if (-not (Test-Path $p)) { throw "Not found: $p" } }
if (-not (Test-Path "$Source\efi\boot\bootaa64.efi")) { throw "$Source has no efi\boot\bootaa64.efi." }
if (-not (Test-Path "$Source\sources\boot.wim"))      { throw "$Source has no sources\boot.wim." }
$null = Get-PartitionNode "esp"

if ($BcdOnly) {
    Invoke-PhoneShell "mkdir -p /tmp/esp; umount /tmp/esp 2>/dev/null; mount -t vfat /dev/block/by-name/esp /tmp/esp" | Out-Null
    try {
        Invoke-Adb push "$Source\efi\microsoft\boot\bcd" /tmp/esp/efi/microsoft/boot/bcd | Out-Null
        Write-Host "Pushed updated BCD."
    } finally { Invoke-PhoneShell "sync; umount /tmp/esp" | Out-Null }
    Write-Host "Reboot -> System, choose internal storage."
    return
}

if ($Trace) {
    if (-not (Test-Path $TraceEfi)) { throw "Not found: $TraceEfi" }
    Invoke-PhoneShell "mkdir -p /tmp/esp; umount /tmp/esp 2>/dev/null; mount -t vfat /dev/block/by-name/esp /tmp/esp" | Out-Null
    try {
        Invoke-PhoneShell "mkdir -p /tmp/esp/EFI/r8s" | Out-Null
        Invoke-Adb push $TraceEfi /tmp/esp/EFI/r8s/R8sTrace.efi | Out-Null
        $nsh = @'
@echo -off
echo " "
echo "=== r8s-woa: self-test, then desktop WinPE UNDER THE TRACER ==="
for %m in fs0 fs1 fs2 fs3 fs4
  if exist %m:\EFI\r8s\R8sDiag.efi then
    %m:\EFI\r8s\R8sDiag.efi
  endif
endfor
for %m in fs0 fs1 fs2 fs3 fs4
  if exist %m:\efi\boot\winpe.efi then
    %m:
    echo "Tracing the desktop boot (last line in r8s-trace.txt = where it died) ..."
    \EFI\r8s\R8sTrace.efi
    echo " "
    echo "Returned, lasterror = %lasterror%. Hold Power to restart."
    goto Done
  endif
endfor
:Done
'@
        $nshPath = Join-Path $Scratch "startup.nsh"
        New-Item -ItemType Directory -Force -Path $Scratch | Out-Null
        [System.IO.File]::WriteAllText($nshPath, ($nsh -replace "`r?`n", "`r`n"), [System.Text.Encoding]::ASCII)
        Invoke-Adb push $nshPath /tmp/esp/startup.nsh | Out-Null
        Write-Host "Tracer restored. Note: R8sTrace loads \efi\boot\winpe.efi (this PE's Boot Manager)."
    } finally { Invoke-PhoneShell "sync; umount /tmp/esp" | Out-Null }
    Write-Host "Reboot -> System, internal storage. Then TWRP + Pull-Logs.ps1 for r8s-trace.txt."
    return
}

# Boot Manager only boots the WIM image whose header boot index is set; without
# it winload.efi "isn't found" (0xc0000225).
& "$PSScriptRoot\..\windows\Set-WimBootable.ps1" -Wim "$Source\sources\boot.wim"

New-Item -ItemType Directory -Force -Path $Scratch | Out-Null
Write-Host "==> Rewriting the ESP with the Shell image"
$raw = Join-Path $Scratch "esp-r8s.img"
$in  = [System.IO.File]::OpenRead($EspImage)
$gz  = [System.IO.Compression.GZipStream]::new($in, [System.IO.Compression.CompressionMode]::Decompress)
$outf = [System.IO.File]::Create($raw)
try { $gz.CopyTo($outf) } finally { $outf.Dispose(); $gz.Dispose(); $in.Dispose() }
Invoke-Adb push $raw /tmp/esp-r8s.img | Out-Null
Remove-Item $raw
Invoke-PhoneShell "umount /tmp/esp 2>/dev/null; dd if=/tmp/esp-r8s.img of=/dev/block/by-name/esp bs=4M; sync; rm -f /tmp/esp-r8s.img" | Out-Null

Invoke-PhoneShell "mkdir -p /tmp/esp; mount -t vfat /dev/block/by-name/esp /tmp/esp" | Out-Null
try {
    Invoke-PhoneShell "mv /tmp/esp/EFI/Boot/bootaa64.efi /tmp/shell.efi" | Out-Null

    Write-Host "==> Copying the desktop PE (~600 MB, several minutes)"
    foreach ($item in Get-ChildItem $Source) {
        Write-Host "   $($item.Name)"
        Invoke-Adb push $item.FullName /tmp/esp/ | Out-Null
    }

    Write-Host "==> Shell first, Boot Manager as winpe.efi; tracer removed"
    Invoke-PhoneShell "mv /tmp/esp/efi/boot/bootaa64.efi /tmp/esp/efi/boot/winpe.efi && cp /tmp/shell.efi /tmp/esp/efi/boot/bootaa64.efi && rm /tmp/shell.efi" | Out-Null
    Invoke-PhoneShell "rm -f /tmp/esp/EFI/r8s/R8sTrace.efi /tmp/esp/r8s-noacpi /tmp/esp/r8s-winlog-new" | Out-Null

    $nsh = @'
@echo -off
echo " "
echo "=== r8s-woa: self-test, then desktop WinPE ==="
for %m in fs0 fs1 fs2 fs3 fs4
  if exist %m:\EFI\r8s\R8sDiag.efi then
    echo "Self-test (saved to %m:\r8s-diag.txt) ..."
    %m:\EFI\r8s\R8sDiag.efi
    if exist %m:\r8s-winlog-new then
      echo " "
      echo "Windows log saved. Hold Vol Up + Power for TWRP, run Pull-Logs.ps1."
      goto Done
    endif
  endif
endfor
for %m in fs0 fs1 fs2 fs3 fs4
  if exist %m:\efi\boot\winpe.efi then
    %m:
    echo "Starting the desktop (loads ~600 MB into RAM, give it a minute) ..."
    \efi\boot\winpe.efi
    echo " "
    echo "Boot Manager returned, lasterror = %lasterror%"
    echo "Hold Power to restart."
    goto Done
  endif
endfor
echo "winpe.efi not found on fs0-fs4"
:Done
'@
    $nshPath = Join-Path $Scratch "startup.nsh"
    [System.IO.File]::WriteAllText($nshPath, ($nsh -replace "`r?`n", "`r`n"), [System.Text.Encoding]::ASCII)
    Invoke-Adb push $nshPath /tmp/esp/startup.nsh | Out-Null

    $ls = Invoke-PhoneShell "ls -l /tmp/esp/efi/boot/ /tmp/esp/sources/ /tmp/esp/startup.nsh 2>&1; true"
    Write-Host $ls
}
finally {
    Invoke-PhoneShell "sync; umount /tmp/esp" | Out-Null
}
Write-Host "`nDone. Reboot -> System, choose internal storage. The desktop loads"
Write-Host "from a ~600 MB image, so expect a black screen for up to a minute first."
