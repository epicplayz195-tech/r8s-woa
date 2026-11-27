<#
.SYNOPSIS
  Rebuild D:\woa\WinPE's boot settings (BCD) from the Windows ISO, with only
  the options Build-WinPE.ps1 sets. Run in an Administrator PowerShell.

.DESCRIPTION
  WinPE booted on 12 Nov, then stopped booting after BCD edits made by hand
  (truncatememory added, then removed). This puts the BCD back to a known
  state:
    1. dumps the current BCD (bcdedit /enum all /v) to D:\woa\logs,
    2. keeps a copy of it next to the original (bcd.before-<time>),
    3. copies a fresh BCD from the ISO and deletes the old bcd.LOG* files
       (stale transaction logs can be replayed onto a new hive),
    4. sets testsigning, nointegritychecks, sos, bootmenupolicy legacy,
    5. dumps the new BCD too, so the two can be compared.
  Then push it with: ..\device\Deploy-DesktopPe.ps1 -BcdOnly -Source D:\woa\WinPE
#>
param(
    [string]$WinPE = "D:\woa\WinPE",
    [string]$Iso,
    [string]$LogDir = "D:\woa\logs"
)
$ErrorActionPreference = "Stop"

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this in an Administrator PowerShell."
}
$bootDir = Join-Path $WinPE "efi\microsoft\boot"
$bcd = Join-Path $bootDir "bcd"
if (-not (Test-Path $bcd)) { throw "No BCD at $bcd." }
if (-not $Iso) {
    $Iso = Get-ChildItem "D:\woa\iso" -Recurse -Filter *.iso -ErrorAction SilentlyContinue |
        Sort-Object Length -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Iso -or -not (Test-Path $Iso)) { throw "Windows ISO not found. Pass -Iso <path>." }
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"

function Save-BcdDump([string]$Store, [string]$File) {
    $out = bcdedit /store $Store /enum all /v 2>&1
    $out | Set-Content $File
    if ($LASTEXITCODE -ne 0) { Write-Warning "bcdedit /enum failed: $out" }
}

Write-Host "==> Saving the current BCD"
Save-BcdDump $bcd (Join-Path $LogDir "bcd-$stamp-before.txt")
Copy-Item $bcd (Join-Path $bootDir "bcd.before-$stamp") -Force

Write-Host "==> Fresh BCD from $Iso"
$img = Mount-DiskImage -ImagePath $Iso -PassThru
try {
    $letter = ($img | Get-Volume).DriveLetter
    if (-not $letter) { throw "The ISO mounted without a drive letter." }
    $src = "${letter}:\efi\microsoft\boot\bcd"
    if (-not (Test-Path $src)) { throw "No BCD in the ISO at $src." }
    Get-ChildItem $bootDir -Filter "bcd.LOG*" -Force -ErrorAction SilentlyContinue | Remove-Item -Force
    Copy-Item $src $bcd -Force
    Set-ItemProperty $bcd -Name IsReadOnly -Value $false
} finally {
    Dismount-DiskImage -ImagePath $Iso | Out-Null
}

foreach ($opt in @(
        @("testsigning", "on"),
        @("nointegritychecks", "on"),
        @("sos", "on"),
        @("bootmenupolicy", "legacy"))) {
    $o = bcdedit /store $bcd /set "{default}" $opt[0] $opt[1] 2>&1
    if ($LASTEXITCODE -ne 0) { throw "bcdedit $($opt[0]) failed: $o" }
}
Save-BcdDump $bcd (Join-Path $LogDir "bcd-$stamp-after.txt")

Write-Host "`nBCD rebuilt. Old one kept as $bootDir\bcd.before-$stamp"
Write-Host "Dumps: $LogDir\bcd-$stamp-before.txt and -after.txt"
Write-Host "Next (phone in TWRP):  ..\device\Deploy-DesktopPe.ps1 -BcdOnly -Source $WinPE"
