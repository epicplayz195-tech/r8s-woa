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

# Run adb without letting Windows PowerShell 5.1 turn its stderr (daemon
# start-up notices, dd progress) into a terminating error. Returns stdout
# lines, stderr lines and the exit code separately.
function Invoke-AdbRaw([string[]]$AdbArgs) {
    if (-not $script:Adb) {
        throw "adb not found. Put platform-tools in $PSScriptRoot\platform-tools or D:\woa\platform-tools, or add adb to PATH."
    }
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $all  = & $script:Adb @AdbArgs 2>&1
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $prev }
    $stdout = @($all | Where-Object { $_ -isnot [System.Management.Automation.ErrorRecord] } | ForEach-Object { "$_" })
    $stderr = @($all | Where-Object { $_ -is    [System.Management.Automation.ErrorRecord] } | ForEach-Object { "$_" })
    [pscustomobject]@{ Out = ($stdout -join "`n").Trim(); Err = ($stderr -join "`n").Trim(); Code = $code }
}

function Invoke-Adb {
    param([Parameter(ValueFromRemainingArguments)][string[]]$AdbArgs)
    $r = Invoke-AdbRaw $AdbArgs
    if ($r.Code -ne 0) { throw "adb $($AdbArgs -join ' ') failed (exit $($r.Code)):`n$($r.Out)`n$($r.Err)" }
    return $r.Out
}

function Invoke-PhoneShell([string]$Command) {
    $r = Invoke-AdbRaw @("shell", $Command)
    if ($r.Code -ne 0) { throw "adb shell '$Command' failed (exit $($r.Code)):`n$($r.Out)`n$($r.Err)" }
    return $r.Out
}

function Assert-R8sInRecovery {
    Write-Host "Using adb: $script:Adb"
    $r = Invoke-AdbRaw @("get-state")
    if ($r.Out -ne "recovery") {
        $devices = (Invoke-AdbRaw @("devices")).Out
        throw "Phone is not in TWRP (adb state: '$($r.Out)' $($r.Err)).`nadb devices:`n$devices`nBoot TWRP, plug in USB, and retry."
    }

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
