<#
.SYNOPSIS
  Prepare a ready-made desktop WinPE (other-pe) for the r8s: inject the r8s
  drivers and mark its boot.wim bootable, WITHOUT replacing its desktop
  startup. Run as Administrator. Nothing on C: is used.

.DESCRIPTION
  Unlike Customize-WinPE.ps1 (which installs our logging startnet and so
  replaces the shell), this leaves the PE's own startup alone so you still
  get its desktop + Device Manager. It only:
    1. mounts boot.wim on an NTFS VHD (D: is exFAT, DISM can't mount there),
    2. copies every driver under $Drivers (r8slog, r8stouch, ...) to X:\r8s\drivers
       and runs X:\r8s\install-drivers.cmd (pnputil) when the desktop starts,
    3. prints winpeshl.ini / startnet.cmd so we can see how it launches the
       desktop (to add background logging next round),
    4. commits and marks the image bootable.
#>
param(
    [string]$Wim     = "D:\woa\other-pe\winpe_desktop_nodrivers\sources\boot.wim",
    [string]$Scratch = "D:\woa\scratch",
    [string]$Drivers = "D:\woa\drivers"
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

Write-Host "==> NTFS work disk $vhd as ${letter}: (boot.wim is ~600 MB, give it room)"
if (Test-Path $vhd) {
    Invoke-DiskPart "select vdisk file=`"$vhd`"`ndetach vdisk noerr"
    Remove-Item $vhd -Force
}
Invoke-DiskPart @"
create vdisk file="$vhd" maximum=8192 type=expandable
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
    Write-Host "==> Mounting boot.wim (a few minutes; it's a big image)"
    dism /Mount-Image /ImageFile:$Wim /Index:1 /MountDir:$mount /ScratchDir:$wscr
    if ($LASTEXITCODE -ne 0) { throw "DISM mount failed ($LASTEXITCODE)." }

    if (Test-Path $Drivers) {
        # DISM /Add-Driver fails on this older image from an x64 host (error 216), so the
        # packages are copied in and installed by pnputil when the desktop starts.
        Write-Host "==> Copying drivers from $Drivers to X:\r8s\drivers"
        $dst = Join-Path $mount "r8s\drivers"
        if (Test-Path $dst) { Remove-Item $dst -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $dst | Out-Null
        Copy-Item (Join-Path $Drivers "*") $dst -Recurse -Force
        Get-ChildItem $dst -Recurse -File | ForEach-Object { "    " + $_.FullName.Substring($dst.Length + 1) }

        $cmd = @'
@echo off
rem r8s-woa: trust the test certificate, then install every driver package in X:\r8s\drivers.
set LOG=X:\r8s\install-log.txt
echo r8s-woa driver install %DATE% %TIME% > %LOG%
for /r X:\r8s\drivers %%c in (*.cer) do (
  certutil -f -addstore Root "%%c" >> %LOG% 2>&1
  certutil -f -addstore TrustedPublisher "%%c" >> %LOG% 2>&1
)
for /r X:\r8s\drivers %%i in (*.inf) do pnputil /add-driver "%%i" /install >> %LOG% 2>&1
echo. >> %LOG%
echo Touch driver state (State 1 = running; see docs\touch.md): >> %LOG%
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RWOA0003" /s >> %LOG% 2>&1
if not "%1"=="/auto" type %LOG%
'@
        [IO.File]::WriteAllText((Join-Path $mount "r8s\install-drivers.cmd"), ($cmd -replace "`r?`n", "`r`n"), [Text.Encoding]::ASCII)

        # Run it when the desktop (explorer) starts: HKLM\...\Run in the image's SOFTWARE hive.
        $hive = Join-Path $mount "Windows\System32\config\SOFTWARE"
        reg load HKLM\R8S_PE_SW $hive | Out-Null
        try {
            reg add "HKLM\R8S_PE_SW\Microsoft\Windows\CurrentVersion\Run" /v r8s-drivers /t REG_SZ /d "cmd /c X:\r8s\install-drivers.cmd /auto" /f | Out-Null
            Write-Host "    install-drivers.cmd runs when the desktop starts (log: X:\r8s\install-log.txt)"
        } finally {
            [gc]::Collect(); Start-Sleep -Seconds 1
            reg unload HKLM\R8S_PE_SW | Out-Null
        }
    } else {
        Write-Warning "No $Drivers folder: desktop PE gets no r8s drivers."
    }

    Write-Host "`n===== how this PE launches its desktop ====="
    foreach ($f in "System32\winpeshl.ini", "System32\startnet.cmd") {
        $p = Join-Path $mount $f
        if (Test-Path $p) {
            Write-Host "--- $f ---"
            Get-Content $p | Out-Host
        } else {
            Write-Host "--- $f : (none) ---"
        }
    }
    Write-Host "============================================`n"
    $ok = $true
}
finally {
    $mode = if ($ok) { "/Commit" } else { "/Discard" }
    Write-Host "==> Unmounting ($mode; the commit takes a while for this image)"
    dism /Unmount-Image /MountDir:$mount $mode /ScratchDir:$wscr
    Invoke-DiskPart "select vdisk file=`"$vhd`"`ndetach vdisk"
    Remove-Item $vhd, $dp -Force -ErrorAction SilentlyContinue
}

& "$PSScriptRoot\Set-WimBootable.ps1" -Wim $Wim
Write-Host "`nDone. Deploy it with:"
Write-Host "  ..\device\Deploy-DesktopPe.ps1 -NewBootMgr     (phone in TWRP; the 23H2 BCD has testsigning on)"
