<#
.SYNOPSIS
  Build ARM64 WinPE boot media for the S20 FE. Run as Administrator.

.DESCRIPTION
  Two sources, pick one:

  -Iso  A Windows 11 ARM64 ISO, build 22621/22631 (22H2/23H2). Index 1 of
        its boot.wim is plain WinPE. Recommended: the same ISO is what you'll
        install full Windows from later. Get one from uupdump.net
        (Windows 11 23H2, arm64).
  -Adk  An installed Windows ADK + WinPE add-on (version 10.1.22621).

  Mu-Silicium only boots Windows 23H2 and older on Exynos, so builds 26100+
  (24H2/25H2) are refused unless you pass -Force.

  Output (default D:\woa\WinPE) is a folder ready for
  Deploy-Windows.ps1 -WinPE. All scratch/mount space stays on D:.

.EXAMPLE
  .\Build-WinPE.ps1 -Iso D:\woa\iso\22631_arm64.iso
  .\Build-WinPE.ps1 -Iso D:\woa\iso\22631_arm64.iso -Drivers D:\woa\drivers
  .\Build-WinPE.ps1 -Adk
#>
param(
    [string]$Iso,
    [switch]$Adk,
    [string]$Out     = "D:\woa\WinPE",
    [string]$Drivers,
    [string]$Scratch = "D:\woa\scratch",
    [switch]$Force
)
$ErrorActionPreference = "Stop"

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this from an elevated PowerShell."
}
if ([bool]$Iso -eq [bool]$Adk) { throw "Pass exactly one of -Iso or -Adk." }
foreach ($p in $Out, $Scratch) {
    if ($p.ToUpper().StartsWith("C:")) { throw "Refusing to use C: ($p)." }
}

# Keep every temp file off C:
New-Item -ItemType Directory -Force -Path $Scratch | Out-Null
$env:TEMP = $Scratch; $env:TMP = $Scratch

function Assert-WinPEWim([string]$Wim, [int]$Index) {
    $img = Get-WindowsImage -ImagePath $Wim -Index $Index
    $build = [int]($img.Version -split "\.")[2]
    $arch  = $img.Architecture   # 12 = ARM64
    Write-Host "WinPE: $($img.ImageName), version $($img.Version), arch $arch"
    if ($arch -ne 12) { throw "$Wim index $Index is not ARM64." }
    if ($build -ge 26100 -and -not $Force) {
        throw "Build $build is 24H2 or newer; Exynos only boots 23H2 (22631) and older. Use a 22621/22631 source, or -Force to try anyway."
    }
}

if (Test-Path $Out) {
    Write-Warning "$Out exists and will be replaced."
    if ((Read-Host "Type YES to continue") -ne "YES") { throw "Aborted." }
    Remove-Item -Recurse -Force $Out
}
New-Item -ItemType Directory -Force -Path "$Out\sources" | Out-Null

if ($Iso) {
    $isoPath = (Resolve-Path $Iso).Path
    $mounted = Mount-DiskImage -ImagePath $isoPath -PassThru
    try {
        $drive = ($mounted | Get-Volume).DriveLetter + ":"
        $srcWim = "$drive\sources\boot.wim"
        if (-not (Test-Path "$drive\efi\boot\bootaa64.efi")) { throw "$Iso is not an ARM64 Windows ISO (no efi\boot\bootaa64.efi)." }
        Assert-WinPEWim $srcWim 1

        Write-Host "==> Copying boot files"
        robocopy "$drive\efi" "$Out\efi" /E /NFL /NDL /NJH /NJS | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)." }
        if (Test-Path "$drive\bootmgr.efi") { Copy-Item "$drive\bootmgr.efi" $Out }

        Write-Host "==> Extracting WinPE (boot.wim index 1)"
        Export-WindowsImage -SourceImagePath $srcWim -SourceIndex 1 `
            -DestinationImagePath "$Out\sources\boot.wim" -CompressionType max `
            -ScratchDirectory $Scratch | Out-Null
    }
    finally { Dismount-DiskImage -ImagePath $isoPath | Out-Null }
}
else {
    $kits = (Get-ItemProperty "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots" -ErrorAction SilentlyContinue).KitsRoot10
    $pe = Join-Path "$kits" "Assessment and Deployment Kit\Windows Preinstallation Environment\arm64"
    if (-not $kits -or -not (Test-Path "$pe\en-us\winpe.wim")) {
        throw "ADK WinPE add-on (arm64) not found. Install ADK + WinPE add-on 10.1.22621, or use -Iso."
    }
    Assert-WinPEWim "$pe\en-us\winpe.wim" 1
    robocopy "$pe\Media" $Out /E /NFL /NDL /NJH /NJS | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)." }
    Copy-Item "$pe\en-us\winpe.wim" "$Out\sources\boot.wim"
}

# Files copied off an ISO are read-only; clear that so BCD/boot.wim can be edited.
Get-ChildItem $Out -Recurse -File | ForEach-Object { $_.IsReadOnly = $false }

# Mount boot.wim once: add drivers (optional) and make crashes stay on screen.
Write-Host "==> Customising WinPE"
$mount = Join-Path $Scratch "mount"
New-Item -ItemType Directory -Force -Path $mount | Out-Null
Mount-WindowsImage -ImagePath "$Out\sources\boot.wim" -Index 1 -Path $mount -ScratchDirectory $Scratch | Out-Null
try {
    if ($Drivers) {
        Write-Host "   adding drivers from $Drivers"
        Add-WindowsDriver -Path $mount -Driver $Drivers -Recurse -ForceUnsigned -ScratchDirectory $Scratch | Out-Null
    }
    # No log can be saved on the phone yet (no UFS driver in Windows), so a
    # blue screen must stay up long enough to read/film: no auto-reboot.
    Write-Host "   blue screens stay on screen (AutoReboot=0)"
    reg load HKLM\R8S_PE_SYS "$mount\Windows\System32\config\SYSTEM" | Out-Null
    try {
        reg add "HKLM\R8S_PE_SYS\ControlSet001\Control\CrashControl" /v AutoReboot /t REG_DWORD /d 0 /f | Out-Null
        reg add "HKLM\R8S_PE_SYS\ControlSet001\Control\CrashControl" /v DisplayParameters /t REG_DWORD /d 1 /f | Out-Null
    } finally {
        [gc]::Collect(); Start-Sleep -Seconds 1
        reg unload HKLM\R8S_PE_SYS | Out-Null
    }
    Dismount-WindowsImage -Path $mount -Save -ScratchDirectory $Scratch | Out-Null
} catch {
    Dismount-WindowsImage -Path $mount -Discard -ScratchDirectory $Scratch -ErrorAction SilentlyContinue | Out-Null
    throw
}

# r8s-woa drivers are test-signed; let WinPE load them.
$bcd = "$Out\efi\microsoft\boot\bcd"
if (-not (Test-Path $bcd)) { throw "No BCD at $bcd." }
foreach ($opt in @(
        @("testsigning", "on"),          # r8s-woa drivers are test-signed
        @("nointegritychecks", "on"),
        @("sos", "on"),                  # print each driver as it loads: shows where boot stops
        @("bootmenupolicy", "legacy"))) {
    $o = bcdedit /store $bcd /set "{default}" $opt[0] $opt[1] 2>&1
    if ($LASTEXITCODE -ne 0) { throw "bcdedit $($opt[0]) failed: $o" }
}
Write-Host "   BCD: test signing, driver list on screen"

$sizeMB = [math]::Round((Get-ChildItem $Out -Recurse -File | Measure-Object Length -Sum).Sum / 1MB)
Write-Host "`nWinPE media ready: $Out ($sizeMB MB)"
Write-Host "Next, with the phone in TWRP:"
Write-Host "  ..\device\Push-Esp.ps1 -Source $Out -Clean"
if ($sizeMB -gt 900) { Write-Warning "This is larger than a 1 GiB ESP; repartition with a bigger -EspSizeMiB." }
