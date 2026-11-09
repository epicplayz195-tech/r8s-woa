<#
.SYNOPSIS
  Zero the Samsung "uh" (Platform Hypervisor Extensions) partition.

.DESCRIPTION
  Samsung's hypervisor traps EL1 MMU registers, which stops Windows from
  booting on Exynos 990 (Mu-Silicium status page, x1s). Erasing "uh" is
  required for Windows. Android will not boot until uh is restored from
  the BL tar (or from the uh.img that Backup-Phone.ps1 saved).
#>
param([string]$BackupDir = "D:\woa\backups")
. "$PSScriptRoot\Common.ps1"

$null = Assert-R8sInRecovery

$saved = Get-ChildItem -Path $BackupDir -Recurse -Filter "uh.img" -ErrorAction SilentlyContinue |
         Select-Object -First 1
if (-not $saved) { throw "No uh.img backup under $BackupDir. Run Backup-Phone.ps1 first." }
Write-Host "Found uh backup: $($saved.FullName)"

Confirm-Danger "This zeroes the 'uh' partition. Android stops booting until it is restored."

Invoke-PhoneShell "dd if=/dev/zero of=/dev/block/by-name/uh bs=4M; sync" | Out-Null
Write-Host "uh erased."
