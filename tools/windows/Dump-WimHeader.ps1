<#
.SYNOPSIS
  Read-only: saves a WIM's header, lookup table and XML data to D:\woa\scratch
  (wimhdr.bin, wimlt.bin, wimxml.bin) for inspection.
#>
param([string]$Wim = "D:\woa\WinPE\sources\boot.wim", [string]$Out = "D:\woa\scratch")
$ErrorActionPreference = "Stop"
$fs = [System.IO.File]::OpenRead($Wim)
try {
    $br  = [System.IO.BinaryReader]::new($fs)
    $hdr = $br.ReadBytes(208)
    [System.IO.File]::WriteAllBytes("$Out\wimhdr.bin", $hdr)
    foreach ($r in @(@{At = 0x30; Name = "wimlt.bin"}, @{At = 0x48; Name = "wimxml.bin"})) {
        $size = [BitConverter]::ToUInt64($hdr, $r.At) -band 0x00FFFFFFFFFFFFFF
        $off  = [BitConverter]::ToUInt64($hdr, $r.At + 8)
        $fs.Position = [int64]$off
        [System.IO.File]::WriteAllBytes("$Out\$($r.Name)", $br.ReadBytes([int][Math]::Min($size, 16MB)))
        Write-Host "$($r.Name): offset $off size $size"
    }
    Write-Host "images $([BitConverter]::ToUInt32($hdr, 0x2C)), boot index $([BitConverter]::ToUInt32($hdr, 0x78)), flags 0x$('{0:X}' -f [BitConverter]::ToUInt32($hdr, 0x10))"
}
finally { $fs.Dispose() }
