<#
.SYNOPSIS
  Debug layout for the ESP: the UEFI Shell boots first and starts Windows
  Boot Manager itself, so any error code stays on screen. Phone in TWRP.

.DESCRIPTION
  1. rewrites the ESP from esp-r8s.img.gz (FAT32 + UEFI Shell),
  2. copies the WinPE folder on top,
  3. renames WinPE's \efi\boot\bootaa64.efi (Boot Manager) to winpe.efi and
     puts the Shell back as bootaa64.efi,
  4. writes a startup.nsh that runs the firmware self-test (R8sDiag.efi,
     prints each service Boot Manager uses before calling it), then
     winpe.efi, and prints its return code if it comes back. No typing.

  On screen afterwards:
    "Running ...winpe.efi" then frozen/black -> Boot Manager hangs.
    "Boot Manager returned, lasterror = ..."  -> it failed; that's the code.
  Logs: \r8s-diag.txt (self-test) and \r8s-trace.txt (every firmware call
  Boot Manager makes) are saved on the ESP; Pull-Logs.ps1 copies them.
    Boot Manager menu / Windows logo         -> it works; continue normally.
#>
param(
    [string]$Source   = "D:\woa\WinPE",
    [string]$EspImage = "D:\woa\Mu-r8s-DEBUG\esp-r8s.img.gz",
    [string]$Scratch  = "D:\woa\scratch",
    [switch]$NoAcpi      # experiment: tracer hides the ACPI tables from Boot Manager
)
. "$PSScriptRoot\Common.ps1"

$null = Assert-R8sInRecovery
if (-not (Test-Path $EspImage) -and (Test-Path "D:\woa\Mu-r8s-RELEASE\esp-r8s.img.gz")) {
    Write-Warning "No DEBUG ESP image at $EspImage; using the RELEASE one (no call trace)."
    $EspImage = "D:\woa\Mu-r8s-RELEASE\esp-r8s.img.gz"
}
foreach ($p in $Source, $EspImage) { if (-not (Test-Path $p)) { throw "Not found: $p" } }
if (-not (Test-Path "$Source\efi\boot\bootaa64.efi")) { throw "$Source has no efi\boot\bootaa64.efi." }
$null = Get-PartitionNode "esp"
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

    Write-Host "==> Copying WinPE (a few minutes)"
    foreach ($item in Get-ChildItem $Source) {
        Write-Host "   $($item.Name)"
        Invoke-Adb push $item.FullName /tmp/esp/ | Out-Null
    }

    Write-Host "==> Shell first, Boot Manager as winpe.efi"
    Invoke-PhoneShell "mv /tmp/esp/efi/boot/bootaa64.efi /tmp/esp/efi/boot/winpe.efi && cp /tmp/shell.efi /tmp/esp/efi/boot/bootaa64.efi && rm /tmp/shell.efi" | Out-Null

    $nsh = @'
@echo -off
echo " "
echo "=== r8s-woa: self-test, then Windows Boot Manager ==="
for %m in fs0 fs1 fs2 fs3 fs4
  if exist %m:\EFI\r8s\R8sDiag.efi then
    echo "Self-test (saved to %m:\r8s-diag.txt) ..."
    %m:\EFI\r8s\R8sDiag.efi
  endif
endfor
for %m in fs0 fs1 fs2 fs3 fs4
  if exist %m:\efi\boot\winpe.efi then
    %m:
    if exist \EFI\r8s\R8sTrace.efi then
      echo "Running Boot Manager under the tracer (DEBUG firmware shows the calls) ..."
      \EFI\r8s\R8sTrace.efi
    else
      echo "Running %m:\efi\boot\winpe.efi ..."
      \efi\boot\winpe.efi
    endif
    echo " "
    echo "Boot Manager returned, lasterror = %lasterror%"
    echo "Film this screen. Hold Power to restart."
    goto Done
  endif
endfor
echo "winpe.efi not found on fs0-fs4"
:Done
'@
    $nshPath = Join-Path $Scratch "startup.nsh"
    # The UEFI Shell reads UCS-2 (UTF-16LE with BOM) or ASCII; use ASCII with CRLF.
    [System.IO.File]::WriteAllText($nshPath, ($nsh -replace "`r?`n", "`r`n"), [System.Text.Encoding]::ASCII)
    Invoke-Adb push $nshPath /tmp/esp/startup.nsh | Out-Null
    if ($NoAcpi) {
        Invoke-PhoneShell "echo 1 > /tmp/esp/r8s-noacpi" | Out-Null
        Write-Host "==> NoAcpi: Boot Manager will start without ACPI tables (experiment)"
    }

    $ls = Invoke-PhoneShell "ls -l /tmp/esp/efi/boot/ /tmp/esp/EFI/r8s/ /tmp/esp/startup.nsh /tmp/esp/sources/ 2>&1; true"
    Write-Host $ls
}
finally {
    Invoke-PhoneShell "sync; umount /tmp/esp" | Out-Null
}
Write-Host "`nDone. Reboot -> System, choose internal storage, and film from the start."
