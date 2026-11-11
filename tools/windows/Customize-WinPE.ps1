<#
.SYNOPSIS
  Puts the r8s-woa startup screen (startnet.cmd) into WinPE's boot.wim.
  Run as Administrator. Nothing on C: is used.

.DESCRIPTION
  (WinPE has no findstr.exe or timeout.exe; the screen uses find and ping.)

  DISM can't mount a WIM on a non-NTFS drive (D: is exFAT), so this makes a
  small NTFS virtual disk in D:\woa\scratch, mounts boot.wim there, writes
  Windows\System32\startnet.cmd, commits, and marks the image bootable again.

  Also adds the r8s drivers (D:\woa\drivers: r8slog = RAM log). At boot,
  WinPE waits 60 s (plug USB in), writes devices, drivers, registry and
  setupapi.dev.log to \\.\R8sLog, then reboots; the firmware saves it as
  \r8s-winlog.txt on the ESP and stops, and Pull-Logs.ps1 fetches it.
  Without the log driver it falls back to a device summary on screen.
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
wpeinit
color 1F
rem WinPE has no timeout.exe or findstr.exe: ping waits, find filters.
set LOG=\\.\R8sLog
cls
echo ================= r8s-woa WinPE =================
echo  Windows is running. Plug USB in now to have it tested.
echo  In 60 s everything is written to the log, then the
echo  phone restarts by itself so the firmware can save it.
ping -n 61 127.0.0.1 >nul
(echo.) > %LOG% 2>nul || goto nolog
call :collect > %LOG% 2>&1
cls
echo ================= r8s-woa WinPE =================
echo  Log written. Restarting in 10 s.
echo  After the restart the firmware saves the log and stops.
echo  Then hold Vol Up + Power for TWRP and run Pull-Logs.ps1.
ping -n 11 127.0.0.1 >nul
wpeutil reboot
goto :eof

:nolog
echo.
echo  !! %LOG% missing: the log driver did not load. Screen only.
ping -n 6 127.0.0.1 >nul
:loop
cls
echo ================= r8s-woa WinPE (no log driver) =========
for /f %%a in ('pnputil /enum-devices /connected ^| find /c "Instance ID"') do echo  Devices found: %%a
echo ---- USB ----
pnputil /enum-devices /connected | find "RWOA"
pnputil /enum-devices /connected /class USB | find "Instance ID"
echo ---- Devices WITHOUT a working driver ----
pnputil /enum-devices /problem | find /v "Class GUID" | find /v "Manufacturer" | find /v "Driver Name" | find /v "Device Description"
ping -n 31 127.0.0.1 >nul
goto loop

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
echo ----- registry: Enum\USB, Enum\ACPI -----
reg query HKLM\SYSTEM\CurrentControlSet\Enum\USB /s
reg query HKLM\SYSTEM\CurrentControlSet\Enum\ACPI /s
echo ----- driver packages -----
pnputil /enum-drivers
echo ----- setupapi.dev.log -----
type X:\Windows\INF\setupapi.dev.log
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
