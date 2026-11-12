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
    2. adds every driver under $Drivers (r8slog, and USB/UFS later),
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
        Write-Host "==> Adding drivers from $Drivers"
        dism /Image:$mount /Add-Driver /Driver:$Drivers /Recurse /ForceUnsigned /ScratchDir:$wscr
        if ($LASTEXITCODE -ne 0) { throw "DISM Add-Driver failed ($LASTEXITCODE)." }
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
Write-Host "  .\Push-EspDebug.ps1 -Source D:\woa\other-pe\winpe_desktop_nodrivers -EspImage D:\woa\Mu-r8s-RELEASE\esp-r8s.img.gz"
