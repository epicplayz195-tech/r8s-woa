<#
.SYNOPSIS
  Puts the r8s-woa startup screen (startnet.cmd) into WinPE's boot.wim.
  Run as Administrator. Nothing on C: is used.

.DESCRIPTION
  (WinPE has no findstr.exe; the screen uses find.exe.)

  DISM can't mount a WIM on a non-NTFS drive (D: is exFAT), so this makes a
  small NTFS virtual disk in D:\woa\scratch, mounts boot.wim there, writes
  Windows\System32\startnet.cmd, commits, and marks the image bootable again.

  The startup screen repeats every 20 s, so devices that appear later (a USB
  keyboard plugged in) show up. If a keyboard works, press Ctrl+C to get a
  normal command prompt.
#>
param(
    [string]$Wim     = "D:\woa\WinPE\sources\boot.wim",
    [string]$Scratch = "D:\woa\scratch"
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

    $cmd = @'
@echo off
wpeinit
color 1F
:loop
cls
echo ================= r8s-woa WinPE =================
echo  Windows is running. Screen refreshes every 20 s.
echo  USB keyboard working? Press Ctrl+C for a prompt.
echo.
for /f %%a in ('pnputil /enum-devices /connected ^| find /c "Instance ID"') do echo  Devices found: %%a
echo.
echo ---- Devices WITHOUT a working driver ----
pnputil /enum-devices /problem | find /v "Class GUID" | find /v "Manufacturer" | find /v "Driver Name"
echo.
echo ---- USB ----
pnputil /enum-devices /connected /class USB | find "Instance ID"
pnputil /enum-devices /connected /class Keyboard | find "Instance ID"
echo.
echo ---- All devices ----
pnputil /enum-devices /connected | find "Instance ID"
timeout /t 20 /nobreak >nul
goto loop
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
