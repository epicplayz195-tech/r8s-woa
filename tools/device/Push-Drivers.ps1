<#
.SYNOPSIS
  Put the drivers in D:\woa\drivers on the phone's esp, with an installer for the installed
  Windows. Phone in TWRP. Then, in Windows: an Administrator command prompt,
      mountvol S: /S
      S:\r8s-install-drivers.cmd
  and restart.
#>
param([string]$Drivers = "D:\woa\drivers")
. "$PSScriptRoot\Common.ps1"

$null = Assert-R8sInRecovery
if (-not (Test-Path (Join-Path $Drivers "r8slog\r8slog.inf"))) { throw "$Drivers has no drivers: unzip r8s-drivers-arm64 there." }
$cmd = @'
@echo off
rem r8s-woa: install (update) the r8s drivers in this Windows. Run as Administrator.
cd /d "%~dp0"
for %%c in (r8s\drivers\*.cer) do (
  certutil -f -addstore Root "%%c" >nul
  certutil -f -addstore TrustedPublisher "%%c" >nul
)
pnputil /add-driver "r8s\drivers\*.inf" /subdirs /install
echo.
echo Done. Restart Windows (Start, Power, Restart) to use the new drivers.
pause
'@
$tmp = Join-Path $env:TEMP "r8s-install-drivers.cmd"
[IO.File]::WriteAllText($tmp, ($cmd -replace "`r?`n", "`r`n"), [Text.Encoding]::ASCII)

Invoke-PhoneShell "umount /tmp/esp 2>/dev/null; mkdir -p /tmp/esp && mount -t vfat /dev/block/by-name/esp /tmp/esp && rm -rf /tmp/esp/r8s && mkdir -p /tmp/esp/r8s" | Out-Null
try {
    Write-Host "==> Copying $Drivers to the esp"
    Invoke-Adb push $Drivers /tmp/esp/r8s/ | Out-Null
    Invoke-PhoneShell "[ -d /tmp/esp/r8s/drivers ] || mv /tmp/esp/r8s/$(Split-Path $Drivers -Leaf) /tmp/esp/r8s/drivers; true" | Out-Null
    Invoke-Adb push $tmp /tmp/esp/r8s-install-drivers.cmd | Out-Null
    Write-Host (Invoke-PhoneShell "ls -R /tmp/esp/r8s | head -30; ls -l /tmp/esp/r8s-install-drivers.cmd")
} finally {
    Invoke-PhoneShell "sync; umount /tmp/esp" | Out-Null
    Remove-Item $tmp -ErrorAction SilentlyContinue
}
Write-Host "`nDone. In Windows, Administrator command prompt:  mountvol S: /S   then   S:\r8s-install-drivers.cmd" -ForegroundColor Green
