<#
.SYNOPSIS
  Copy Windows' own setup and driver logs off the phone's "win" partition (read-only).
  Phone in TWRP. Saves them to D:\woa\logs\windows-<time>\.

  Panther\setuperr.log / setupact.log  - what Windows setup did and where it failed
  INF\setupapi.dev.log / .offline.log  - every driver install
  System32\winevt\Logs\System.evtx     - the System event log (open it in Event Viewer)
  Minidump\*.dmp                       - crash dumps, if any
#>
param([string]$Out = ("D:\woa\logs\windows-" + (Get-Date -Format "yyyyMMdd-HHmmss")))
. "$PSScriptRoot\Common.ps1"

$null = Assert-R8sInRecovery
$null = Get-PartitionNode "win"
New-Item -ItemType Directory -Force -Path $Out | Out-Null

$m = Invoke-PhoneShell ("umount /tmp/w 2>/dev/null; mkdir -p /tmp/w; " +
    "if mount -t ntfs3 -o ro /dev/block/by-name/win /tmp/w 2>/dev/null; then echo ntfs3; " +
    "elif mount -t ntfs -o ro /dev/block/by-name/win /tmp/w 2>/dev/null; then echo ntfs; " +
    "elif ntfs-3g -o ro /dev/block/by-name/win /tmp/w 2>/dev/null; then echo ntfs-3g; " +
    "else echo none; cat /proc/filesystems | grep -i ntfs; which ntfs-3g; fi")
Write-Host "mount: $m"
if ($m -match '^none') { throw "TWRP can't mount NTFS here: $m" }

try {
    $items = @(
        "Windows/Panther",
        "Windows/INF/setupapi.dev.log",
        "Windows/INF/setupapi.offline.log",
        "Windows/INF/setupapi.setup.log",
        "Windows/System32/winevt/Logs/System.evtx",
        "Windows/System32/winevt/Logs/Setup.evtx",
        "Windows/System32/winevt/Logs/Application.evtx",
        "Windows/Minidump",
        "Windows/MEMORY.DMP"
    )
    foreach ($i in $items) {
        $exists = Invoke-PhoneShell "[ -e '/tmp/w/$i' ] && echo yes || echo no"
        if ($exists -ne "yes") { Write-Host "   (none) $i"; continue }
        if ($i -eq "Windows/MEMORY.DMP") {
            Write-Host ("   {0}: " -f $i) -NoNewline
            Write-Host (Invoke-PhoneShell "ls -l '/tmp/w/$i'")
            Write-Host "     (full memory dump: not copied, too big; say if you want it)"
            continue
        }
        Write-Host "   $i"
        $r = Invoke-AdbRaw @("pull", "/tmp/w/$i", $Out)
        if ($r.Code -ne 0) { Write-Warning "     pull failed: $($r.Err)" }
    }
} finally {
    Invoke-PhoneShell "umount /tmp/w 2>/dev/null; true" | Out-Null
}
Write-Host "`nSaved to $Out" -ForegroundColor Green
