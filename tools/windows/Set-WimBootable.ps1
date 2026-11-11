<#
.SYNOPSIS
  Marks image 1 of a single-image WIM (WinPE boot.wim) as bootable by
  editing the WIM header directly.

.DESCRIPTION
  Windows Boot Manager only starts the image named by the header's boot index
  from a ramdisk; with boot index 0 it reports winload.efi as not found
  (0xc0000225). DISM's /Bootable doesn't always set it, so this copies image
  1's metadata resource into the header's boot-metadata slot and sets the
  boot index to 1. Only the 28 header bytes at 0x60-0x7B are changed.
#>
param([string]$Wim = "D:\woa\WinPE\sources\boot.wim")
$ErrorActionPreference = "Stop"

$fs = [System.IO.File]::Open($Wim, 'Open', 'ReadWrite', 'None')
try {
    $br = [System.IO.BinaryReader]::new($fs)
    $hdr = $br.ReadBytes(208)
    if ([System.Text.Encoding]::ASCII.GetString($hdr, 0, 5) -ne "MSWIM") { throw "$Wim is not a WIM file." }
    $images = [BitConverter]::ToUInt32($hdr, 0x2C)
    $bootIx = [BitConverter]::ToUInt32($hdr, 0x78)
    Write-Host "Images: $images, boot index now: $bootIx"
    if ($images -ne 1) { throw "Expected a single-image WIM, found $images images." }

    # Offset (lookup) table resource header at 0x30: 7-byte size, flags, offset, original size.
    $ltSize  = [BitConverter]::ToUInt64($hdr, 0x30) -band 0x00FFFFFFFFFFFFFF
    $ltFlags = $hdr[0x37]
    $ltOff   = [BitConverter]::ToUInt64($hdr, 0x38)
    if ($ltFlags -band 0x04) { throw "Lookup table is compressed; not supported." }
    $fs.Position = [int64]$ltOff
    $lt = $br.ReadBytes([int]$ltSize)

    # Lookup entries are 50 bytes: 24-byte resource header, part (2), refcount (4), SHA-1 (20).
    $meta = @()
    for ($i = 0; $i + 50 -le $lt.Length; $i += 50) {
        if ($lt[$i + 7] -band 0x02) { $meta += $i }   # RESHDR_FLAG_METADATA
    }
    if ($meta.Count -ne 1) { throw "Expected 1 metadata resource, found $($meta.Count)." }

    $fs.Position = 0x60
    $fs.Write($lt, $meta[0], 24)                       # boot metadata = image 1's metadata
    $fs.Write([BitConverter]::GetBytes([uint32]1), 0, 4)  # boot index = 1
    $fs.Flush()

    $fs.Position = 0x78
    Write-Host "Boot index now: $([BitConverter]::ToUInt32($br.ReadBytes(4), 0))"
}
finally { $fs.Dispose() }
