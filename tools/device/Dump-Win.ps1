<#
.SYNOPSIS
  Copy the phone's "win" partition to the PC as an r8s-sparse file (only blocks that hold
  data, ~11 GB), read-only on the phone. Phone in TWRP. Used to read Windows' logs when
  TWRP's NTFS driver can't (4 KB sectors with 1 KB MFT records).
#>
param([string]$Out = "D:\woa\scratch\win-back.sparse")
. "$PSScriptRoot\Common.ps1"
. "$PSScriptRoot\AdbStream.ps1"

$null = Assert-R8sInRecovery
$win = Get-PartitionNode "win"
New-Item -ItemType Directory -Force -Path (Split-Path $Out) | Out-Null
$null = Dismount-PhoneFilesystems
Install-SparseHelper
Write-Host "==> Copying $win to $Out (only the blocks that hold data)"
$t = Receive-AdbStream "/tmp/r8s-sparse dump $win 2>/tmp/r8s_err" $Out "Copying win"
$err = Invoke-PhoneShell "cat /tmp/r8s_err; rm -f /tmp/r8s_err"
$v = [R8sSparse]::Verify($Out)
Write-Host ("    {0:N1} GB in {1:mm\:ss}, {2} chunks, digest {3} ({4})" -f ((Get-Item $Out).Length / 1GB), $t, $v.Chunks, $v.Digest, ($err -replace "`n", "; "))
Write-Host "`nDone: $Out" -ForegroundColor Green
