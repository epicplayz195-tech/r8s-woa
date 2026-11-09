<#
.SYNOPSIS
  Build Mu-Silicium UEFI for the Galaxy S20 FE (r8s) natively on Windows.

.DESCRIPTION
  Needs Git, Python 3.11+, LLVM, VS Build Tools and GnuWin32 make on PATH
  (see Mu-Silicium's Building.md). The ~5 GB source tree and build output go
  to D:\woa by default to keep them off C:.

.EXAMPLE
  .\firmware\tools\build.ps1
  .\firmware\tools\build.ps1 -WorkDir E:\woa -Target DEBUG
#>
param(
    [string]$WorkDir = "D:\woa",
    [string]$MuRef   = "main",
    [ValidateSet("RELEASE","DEBUG")][string]$Target = "RELEASE"
)
$ErrorActionPreference = "Stop"

if ($WorkDir.ToUpper().StartsWith("C:")) {
    Write-Warning "WorkDir is on C:. The source tree is ~5 GB; consider another drive."
}

$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$Mu   = Join-Path $WorkDir "Mu-Silicium"
$Out  = Join-Path $WorkDir "out"
New-Item -ItemType Directory -Force -Path $WorkDir, $Out | Out-Null

# Keep pip's cache off C: too
$env:PIP_CACHE_DIR = Join-Path $WorkDir "pip-cache"

if (-not (Test-Path (Join-Path $Mu ".git"))) {
    Write-Host "==> Cloning Mu-Silicium (~5 GB)"
    git clone --recursive https://github.com/Project-Silicium/Mu-Silicium.git $Mu
}

Push-Location $Mu
try {
    git fetch --all --tags --quiet
    git checkout --quiet $MuRef
    git submodule update --init --recursive

    Write-Host "==> Resetting r8s files and applying overlay"
    git checkout -- Platforms/Samsung/r8sPkg
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "Silicium-ACPI\Platforms\Samsung\r8s"
    Copy-Item -Recurse -Force (Join-Path $Root "firmware\overlay\*") $Mu

    python -m pip install -r pip-requirements.txt
    python build_uefi.py -d r8s -r $Target
    if ($LASTEXITCODE -ne 0) { throw "build_uefi.py failed ($LASTEXITCODE)" }

    Copy-Item -Force "Mu-r8s.img" $Out
    $hash = (Get-FileHash (Join-Path $Out "Mu-r8s.img") -Algorithm SHA256).Hash
    Write-Host "==> Done: $Out\Mu-r8s.img  SHA256 $hash"
}
finally { Pop-Location }
