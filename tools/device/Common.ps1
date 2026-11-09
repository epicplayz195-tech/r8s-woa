# Shared helpers for the r8s-woa device scripts. Dot-source, don't run.
# All scripts talk to the phone over adb while it is booted into TWRP.

$ErrorActionPreference = "Stop"
$script:DefaultDataDir = "D:\woa"

# adb: PATH first, then a platform-tools folder next to these scripts or in D:\woa.
$script:Adb = (Get-Command adb -ErrorAction SilentlyContinue).Source
if (-not $script:Adb) {
    foreach ($dir in @("$PSScriptRoot\platform-tools", "$PSScriptRoot\..\..\platform-tools", "$script:DefaultDataDir\platform-tools")) {
        if (Test-Path "$dir\adb.exe") { $script:Adb = (Resolve-Path "$dir\adb.exe").Path; break }
    }
}

function Invoke-Adb {
    param([Parameter(ValueFromRemainingArguments)][string[]]$AdbArgs)
    $out = & $script:Adb @AdbArgs 2>&1
    if ($LASTEXITCODE -ne 0) { throw "adb $($AdbArgs -join ' ') failed:`n$out" }
    return $out
}

function Invoke-PhoneShell([string]$Command) {
    $out = & $script:Adb shell $Command 2>&1
    if ($LASTEXITCODE -ne 0) { throw "adb shell '$Command' failed:`n$out" }
    return ($out | Out-String).Trim()
}

function Assert-R8sInRecovery {
    if (-not $script:Adb) {
        throw "adb not found. Put platform-tools in $PSScriptRoot\platform-tools or D:\woa\platform-tools, or add adb to PATH."
    }
    $state = (& $script:Adb get-state 2>&1 | Out-String).Trim()
    if ($state -ne "recovery") { throw "Phone is not in TWRP (adb state: '$state'). Boot TWRP and retry." }

    $device = Invoke-PhoneShell "getprop ro.product.device"
    if ($device -notmatch "r8s") { throw "Connected device is '$device', not r8s. Refusing to continue." }

    $bl = Invoke-PhoneShell "getprop ro.boot.bootloader"
    Write-Host "Device: $device  Bootloader: $bl"
    return $bl
}

function Get-PartitionNode([string]$Name) {
    $node = Invoke-PhoneShell "readlink -f /dev/block/by-name/$Name"
    if (-not $node -or $node -notmatch "^/dev/block/") { throw "Partition '$Name' not found." }
    return $node
}

function Confirm-Danger([string]$What) {
    Write-Warning $What
    $answer = Read-Host "Type YES to continue"
    if ($answer -ne "YES") { throw "Aborted by user." }
}
