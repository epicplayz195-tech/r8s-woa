<#
.SYNOPSIS
  Copy the boot-test logs from the phone's ESP to D:\woa\logs and show them.
  Phone must be in TWRP.

.DESCRIPTION
  r8s-diag.txt  - firmware self-test (R8sDiag.efi)
  r8s-trace.txt - every firmware call Windows Boot Manager made (R8sTrace.efi),
                  written line by line, so it shows the last call before a hang.
  r8s-winlog.txt - what WinPE logged (devices, drivers, USB), handed to the
                  firmware through RAM across a reboot and saved by R8sDiag.
                  Pulling it also clears the "new Windows log" flag, so the
                  next boot goes on to Windows again.
#>
param([string]$OutDir = "D:\woa\logs")
. "$PSScriptRoot\Common.ps1"

$null = Assert-R8sInRecovery
$null = Get-PartitionNode "esp"
$dest = Join-Path $OutDir (Get-Date -Format "yyyyMMdd-HHmmss")
New-Item -ItemType Directory -Force -Path $dest | Out-Null

Invoke-PhoneShell "mkdir -p /tmp/esp; umount /tmp/esp 2>/dev/null; mount -t vfat /dev/block/by-name/esp /tmp/esp" | Out-Null
try {
    foreach ($f in "r8s-diag.txt", "r8s-trace.txt", "r8s-winlog.txt") {
        $exists = Invoke-PhoneShell "[ -f /tmp/esp/$f ] && echo yes || echo no"
        if ($exists -ne "yes") { Write-Host "($f not found on the ESP)"; continue }
        Invoke-Adb pull "/tmp/esp/$f" (Join-Path $dest $f) | Out-Null
    }
    Invoke-PhoneShell "rm -f /tmp/esp/r8s-winlog-new; sync" | Out-Null
}
finally { Invoke-PhoneShell "umount /tmp/esp" | Out-Null }

foreach ($f in Get-ChildItem $dest -File) {
    Write-Host "`n===== $($f.Name) =====" -ForegroundColor Cyan
    # The Shell writes UCS-2 or ASCII depending on version; let PowerShell detect it.
    Get-Content $f.FullName | Select-Object -Last 80 | Out-Host
}
Write-Host "`nSaved in $dest (the full files; above are the last 80 lines of each)."
