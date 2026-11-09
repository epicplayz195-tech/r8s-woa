<#
.SYNOPSIS
  Apply Windows (or copy WinPE) to drive letters the PC can see.
  NOTE: Samsung builds of Silicium have no Mass Storage, so this can't reach
  the phone directly yet. For WinPE use tools\device\Push-Esp.ps1 (TWRP).
  Run as Administrator on your PC.

.DESCRIPTION
  1. In the UEFI menu on the phone, start Mass Storage and plug into the PC.
  2. In Disk Management, give the phone's 'esp' partition a letter (e.g. S)
     and the 'win' partition a letter (e.g. W).
  3. Run this script.

  Full Windows: formats W: NTFS, applies install.wim/esd, injects drivers,
  writes boot files to S: and turns on test signing (r8s-woa drivers are
  test-signed). WinPE (-WinPE): copies a WinPE media folder to S: only.

  Scratch space and logs go to D:\woa by default.

.EXAMPLE
  .\Deploy-Windows.ps1 -Image E:\sources\install.wim -Index 6 -Esp S -Win W -Drivers .\drivers\out
  .\Deploy-Windows.ps1 -WinPE D:\woa\WinPE -Esp S
#>
param(
    [string]$Image,
    [int]$Index = 1,
    [string]$WinPE,
    [Parameter(Mandatory)][ValidatePattern("^[A-Za-z]$")][string]$Esp,
    [ValidatePattern("^[A-Za-z]$")][string]$Win,
    [string]$Drivers,
    [string]$Scratch = "D:\woa\scratch"
)
$ErrorActionPreference = "Stop"

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this from an elevated PowerShell."
}
foreach ($l in @($Esp, $Win) | Where-Object { $_ }) {
    if ($l.ToUpper() -eq "C") { throw "Refusing to touch C:." }
}
New-Item -ItemType Directory -Force -Path $Scratch | Out-Null
$log = Join-Path $Scratch "dism-$(Get-Date -Format yyyyMMdd-HHmmss).log"

function Confirm-Danger([string]$What) {
    Write-Warning $What
    if ((Read-Host "Type YES to continue") -ne "YES") { throw "Aborted." }
}

$espRoot = "$($Esp.ToUpper()):\"
if (-not (Test-Path $espRoot)) { throw "$espRoot is not mounted." }

if ($WinPE) {
    if (-not (Test-Path (Join-Path $WinPE "EFI\Boot\bootaa64.efi"))) {
        throw "$WinPE does not look like ARM64 WinPE media (no EFI\Boot\bootaa64.efi)."
    }
    Confirm-Danger "Copy WinPE onto $espRoot (existing EFI files there are overwritten)."
    robocopy $WinPE $espRoot /E /NFL /NDL /NJH | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)." }
    Write-Host "WinPE copied. Reboot the phone and pick it from the UEFI boot menu."
    return
}

if (-not $Image -or -not $Win) { throw "Full Windows needs -Image and -Win (or use -WinPE)." }
if (-not (Test-Path $Image)) { throw "Image not found: $Image" }

$info = dism /English /Get-ImageInfo /ImageFile:"$Image" /Index:$Index
if (($info | Out-String) -notmatch "Architecture : arm64") { throw "Index $Index of $Image is not ARM64." }

$winRoot = "$($Win.ToUpper()):\"
$vol = Get-Volume -DriveLetter $Win
Confirm-Danger "Format $winRoot ($([math]::Round($vol.Size/1GB,1)) GB) as NTFS and install Windows on it."

Format-Volume -DriveLetter $Win -FileSystem NTFS -NewFileSystemLabel "Windows" -Confirm:$false | Out-Null

dism /Apply-Image /ImageFile:"$Image" /Index:$Index /ApplyDir:$winRoot /ScratchDir:"$Scratch" /LogPath:"$log"
if ($LASTEXITCODE -ne 0) { throw "DISM apply failed, see $log" }

if ($Drivers) {
    dism /Image:$winRoot /Add-Driver /Driver:"$Drivers" /Recurse /ForceUnsigned /ScratchDir:"$Scratch" /LogPath:"$log"
    if ($LASTEXITCODE -ne 0) { throw "DISM add-driver failed, see $log" }
}

bcdboot "$($winRoot)Windows" /s "$($Esp.ToUpper()):" /f UEFI
if ($LASTEXITCODE -ne 0) { throw "bcdboot failed." }

$bcd = "$($espRoot)EFI\Microsoft\Boot\BCD"
bcdedit /store $bcd /set "{default}" testsigning on        | Out-Null
bcdedit /store $bcd /set "{default}" nointegritychecks on  | Out-Null
bcdedit /store $bcd /set "{default}" recoveryenabled no    | Out-Null
bcdedit /store $bcd /set "{bootmgr}" displaybootmenu no    | Out-Null

Write-Host "Windows deployed. Eject the phone, reboot, and boot from the UEFI menu."
Write-Host "Note: without an Exynos 990 UFS driver, Windows can't see its own disk after"
Write-Host "boot handoff yet. See docs/driver-plan.md."
