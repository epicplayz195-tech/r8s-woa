<#
.SYNOPSIS
  Copy a folder (e.g. D:\woa\WinPE) onto the phone's 'esp' partition from
  TWRP. Replaces UEFI Mass Storage, which Samsung builds of Silicium don't have.

.DESCRIPTION
  Mounts /dev/block/by-name/esp (FAT32) in TWRP, optionally clears it,
  pushes the folder's contents, checks the file count and sizes, unmounts.

.EXAMPLE
  .\Push-Esp.ps1 -Source D:\woa\WinPE -Clean
#>
param(
    [Parameter(Mandatory)][string]$Source,
    [switch]$Clean
)
. "$PSScriptRoot\Common.ps1"

$null = Assert-R8sInRecovery
if (-not (Test-Path $Source -PathType Container)) { throw "Source folder not found: $Source" }
$src = (Resolve-Path $Source).Path
$null = Get-PartitionNode "esp"   # throws if missing (run Partition-Phone.ps1 and reboot TWRP)

$files = Get-ChildItem $src -Recurse -File
$bytes = ($files | Measure-Object Length -Sum).Sum
$espSize = [int64](Invoke-PhoneShell "blockdev --getsize64 /dev/block/by-name/esp")
Write-Host ("{0} files, {1:N0} MB to copy; ESP is {2:N0} MB" -f $files.Count, ($bytes/1MB), ($espSize/1MB))
if ($bytes -gt $espSize * 0.95) { throw "Too big for the ESP." }

Invoke-PhoneShell "mkdir -p /tmp/esp; umount /tmp/esp 2>/dev/null; mount -t vfat /dev/block/by-name/esp /tmp/esp" | Out-Null
try {
    if ($Clean) {
        Confirm-Danger "Delete everything currently on the ESP?"
        Invoke-PhoneShell "rm -rf /tmp/esp/* /tmp/esp/.[!.]* 2>/dev/null; true" | Out-Null
    }
    Write-Host "==> Pushing (this can take a few minutes)"
    # Push each top-level item so the folder's contents land at the ESP root.
    foreach ($item in Get-ChildItem $src) {
        Write-Host "   $($item.Name)"
        Invoke-Adb push $item.FullName /tmp/esp/ | Out-Null
    }
    Invoke-PhoneShell "sync" | Out-Null

    $remoteCount = [int](Invoke-PhoneShell "find /tmp/esp -type f | wc -l")
    $remoteKB = [int64](Invoke-PhoneShell "du -sk /tmp/esp | cut -f1")
    Write-Host "ESP now holds $remoteCount files, $([math]::Round($remoteKB/1024)) MB"
    if ($remoteCount -lt $files.Count) { throw "Only $remoteCount of $($files.Count) files arrived." }
}
finally {
    Invoke-PhoneShell "sync; umount /tmp/esp" | Out-Null
}
Write-Host "Done. Reboot to the UEFI and boot internal storage."
