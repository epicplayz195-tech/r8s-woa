<#
.SYNOPSIS
  Puts the r8s-woa startup screen (startnet.cmd) into WinPE's boot.wim.
  Run as Administrator. Nothing on C: is used.

.DESCRIPTION
  (WinPE has no findstr.exe or timeout.exe, and ping returns at once without
  networking: the script waits by watching %TIME%.)

  DISM can't mount a WIM on a non-NTFS drive (D: is exFAT), so this makes a
  small NTFS virtual disk in D:\woa\scratch, mounts boot.wim there, writes
  Windows\System32\startnet.cmd, commits, and marks the image bootable again.

  Also adds the r8s drivers (D:\woa\drivers: r8slog = RAM log). At boot the
  script writes to X:\Windows\r8s-log.txt (USB state every 15 s for 60 s, then
  devices, drivers, registry and setupapi.dev.log); r8slog.sys copies that
  file into the RAM log every 5 s. The last screen shows the log driver's and
  USB controller's status; after a manual restart the firmware saves the
  log as \r8s-winlog.txt on the ESP and Pull-Logs.ps1 fetches it. The log is
  also copied after every step to r8s-winlog.txt on any drive Windows mounts
  (a USB flash drive). WinPE no longer reboots by itself: Windows' restart
  ends in INTERNAL_POWER_ERROR on this port, so the last screen asks for a
  manual restart.
#>
param(
    [string]$Wim     = "D:\woa\WinPE\sources\boot.wim",
    [string]$Scratch = "D:\woa\scratch",
    [string]$Drivers = "D:\woa\drivers"       # r8s-drivers-arm64 build (r8slog, ...)
)
$ErrorActionPreference = "Stop"

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this in an Administrator PowerShell."
}
if (-not (Test-Path $Wim)) { throw "Not found: $Wim" }
New-Item -ItemType Directory -Force -Path $Scratch | Out-Null

$vhd  = Join-Path $Scratch "wimmount.vhdx"
$dp   = Join-Path $Scratch "diskpart.txt"
$used = (Get-PSDrive -PSProvider FileSystem).Name
$letter = [char[]]"WVUTSRQPONMLK" | Where-Object { $used -notcontains [string]$_ } | Select-Object -First 1
if (-not $letter) { throw "No free drive letter." }

function Invoke-DiskPart([string]$Script) {
    Set-Content -Path $dp -Value $Script -Encoding ASCII
    $out = diskpart /s $dp
    if ($LASTEXITCODE -ne 0) { $out | Write-Host; throw "diskpart failed." }
}

Write-Host "==> NTFS work disk $vhd as ${letter}:"
if (Test-Path $vhd) {
    Invoke-DiskPart "select vdisk file=`"$vhd`"`ndetach vdisk noerr"
    Remove-Item $vhd -Force
}
Invoke-DiskPart @"
create vdisk file="$vhd" maximum=6144 type=expandable
select vdisk file="$vhd"
attach vdisk
create partition primary
format fs=ntfs quick label=wimmount
assign letter=$letter
"@

$mount = "${letter}:\mount"
$wscr  = "${letter}:\scratch"
New-Item -ItemType Directory -Force -Path $mount, $wscr | Out-Null
$ok = $false
try {
    Write-Host "==> Mounting boot.wim (a minute or two)"
    dism /Mount-Image /ImageFile:$Wim /Index:1 /MountDir:$mount /ScratchDir:$wscr
    if ($LASTEXITCODE -ne 0) { throw "DISM mount failed ($LASTEXITCODE)." }

    if (Test-Path $Drivers) {
        Write-Host "==> Adding drivers from $Drivers"
        dism /Image:$mount /Add-Driver /Driver:$Drivers /Recurse /ForceUnsigned /ScratchDir:$wscr
        if ($LASTEXITCODE -ne 0) { throw "DISM Add-Driver failed ($LASTEXITCODE)." }
    } else {
        Write-Warning "No $Drivers folder: WinPE gets no r8s drivers (no RAM log)."
    }

    $cmd = @'
@echo off
rem r8s-woa WinPE startup. WinPE has no timeout.exe/findstr.exe and ping
rem returns at once without networking: waits use the clock (%TIME%).
rem Everything goes to X:\Windows\r8s-log.txt; r8slog.sys copies new text
rem from it into the RAM log every 5 s.
set LOGF=%SystemRoot%\r8s-log.txt
echo [startnet] started %DATE% %TIME% > %LOGF%
wpeinit
echo [startnet] wpeinit done %TIME% >> %LOGF%
color 1F
cls
echo ================= r8s-woa WinPE =================
echo  Windows is running. Logging for about 2 minutes.
echo  Leave the flash drive plugged in. Don't touch the phone.
echo.
for %%i in (1 2 3 4) do (
  call :sleep 15
  echo  USB check %%i of 4 ...
  echo [startnet] USB check %%i %TIME% >> %LOGF%
  pnputil /enum-devices /connected /class USB >> %LOGF% 2>&1
  pnputil /enum-devices /problem >> %LOGF% 2>&1
  call :tousb
)
echo  Writing the full log ...
call :collect >> %LOGF% 2>&1
echo [startnet] full log written >> %LOGF%
call :tousb
cls
echo ================= r8s-woa WinPE =================
echo  Log written%USBMSG%.
echo  Log driver (ACPI\RWOA0002) status:
pnputil /enum-devices /deviceid "ACPI\RWOA0002" | find /i "Status"
pnputil /enum-devices /deviceid "ACPI\RWOA0002" | find /i "Problem"
echo  USB controller (ACPI\RWOA0001) status:
pnputil /enum-devices /deviceid "ACPI\RWOA0001" | find /i "Status"
echo  USB devices found:
pnputil /enum-devices /connected /class USB | find /c "Instance ID"
echo.
echo  DONE. Hold Power (or Vol Down + Power) to restart.
echo  (Windows' own restart crashes on this phone for now.)
echo [startnet] done, waiting for a manual restart >> %LOGF%
call :tousb
:idle
call :sleep 3600
goto idle

:tousb
rem Copy the log to every drive except X: (WinPE) - a USB flash drive if
rem Windows mounted one. Sets USBMSG for the last screen.
set USBMSG=, NO flash drive found
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (
  if exist %%d:\ (
    copy /y %LOGF% %%d:\r8s-winlog.txt >nul 2>&1 && set USBMSG= and copied to the flash drive
  )
)
goto :eof

:sleep
rem call :sleep N  -  waits N seconds using the clock
setlocal
set t=%TIME: =0%
set /a start=(1%t:~0,2%-100)*3600+(1%t:~3,2%-100)*60+(1%t:~6,2%-100)
:sleep_loop
set t=%TIME: =0%
set /a now=(1%t:~0,2%-100)*3600+(1%t:~3,2%-100)*60+(1%t:~6,2%-100)
if %now% lss %start% set /a now+=86400
set /a done=now-start
if %done% lss %1 goto sleep_loop
endlocal
goto :eof

:collect
echo ===== r8s-woa WinPE log =====
ver
echo %DATE% %TIME%
echo.
echo ----- devices (connected) -----
pnputil /enum-devices /connected
echo ----- devices with problems -----
pnputil /enum-devices /problem
echo ----- USB controller / log device -----
pnputil /enum-devices /deviceid "ACPI\RWOA0001" /drivers
pnputil /enum-devices /deviceid "ACPI\RWOA0002" /drivers
pnputil /enum-devices /class USB /drivers
echo ----- disks and volumes (diskpart) -----
(echo list disk& echo list volume) > %TEMP%\dp.txt
diskpart /s %TEMP%\dp.txt
echo ----- registry: Enum\USB, Enum\ACPI -----
reg query HKLM\SYSTEM\CurrentControlSet\Enum\USB /s
reg query HKLM\SYSTEM\CurrentControlSet\Enum\ACPI /s
echo ----- driver packages -----
pnputil /enum-drivers
echo ----- setupapi.dev.log -----
type %SystemRoot%\INF\setupapi.dev.log
echo ===== end =====
exit /b 0
'@
    [IO.File]::WriteAllText("$mount\Windows\System32\startnet.cmd", ($cmd -replace "`r?`n", "`r`n"), [Text.Encoding]::ASCII)
    Write-Host "==> startnet.cmd written"
    $ok = $true
}
finally {
    $mode = if ($ok) { "/Commit" } else { "/Discard" }
    Write-Host "==> Unmounting ($mode)"
    dism /Unmount-Image /MountDir:$mount $mode /ScratchDir:$wscr
    Invoke-DiskPart "select vdisk file=`"$vhd`"`ndetach vdisk"
    Remove-Item $vhd, $dp -Force -ErrorAction SilentlyContinue
}

& "$PSScriptRoot\Set-WimBootable.ps1" -Wim $Wim
Write-Host "`nDone. Next: tools\device\Push-EspDebug.ps1 (phone in TWRP)."
