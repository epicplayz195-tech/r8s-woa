# Binary-safe streaming over adb for big partition images. Dot-source after
# Common.ps1. Windows PowerShell 5.1 mangles binary data piped through '|' or
# '>' from native programs, so these talk to adb.exe through .NET streams.

# Join the shell command into one argv element for adb.exe (MSVCRT parsing).
# Commands passed here must not contain double quotes.
function Start-AdbProcess([string]$Mode, [string]$ShellCmd, [switch]$Stdin, [switch]$Stdout) {
    if ($ShellCmd.Contains('"')) { throw "Internal error: shell command contains a double quote." }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $script:Adb
    $psi.Arguments = "$Mode `"$ShellCmd`""
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardInput = [bool]$Stdin
    return [System.Diagnostics.Process]::Start($psi)
}

function Format-Size([double]$Bytes) {
    if ($Bytes -ge 1GB) { return "{0:N2} GB" -f ($Bytes / 1GB) }
    if ($Bytes -ge 1MB) { return "{0:N1} MB" -f ($Bytes / 1MB) }
    return "{0:N0} KB" -f ($Bytes / 1KB)
}

# Run ShellCmd on the phone (adb exec-out) and save its stdout to OutFile.
function Receive-AdbStream([string]$ShellCmd, [string]$OutFile, [string]$Label) {
    $drive = New-Object System.IO.DriveInfo ([IO.Path]::GetPathRoot([IO.Path]::GetFullPath($OutFile)))
    $p = Start-AdbProcess "exec-out" $ShellCmd
    $err = $p.StandardError.ReadToEndAsync()
    $fs = [System.IO.File]::Create($OutFile)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try {
        $task = $p.StandardOutput.BaseStream.CopyToAsync($fs, 4MB)
        while (-not $task.Wait(1000)) {
            $mbs = $fs.Length / 1MB / [math]::Max($sw.Elapsed.TotalSeconds, 1)
            Write-Progress -Activity $Label -Status ("{0} received, {1:N0} MB/s, {2} free on the PC" -f (Format-Size $fs.Length), $mbs, (Format-Size $drive.AvailableFreeSpace))
            if ($drive.AvailableFreeSpace -lt 1GB) {
                try { $p.Kill() } catch {}
                throw "The PC's drive $($drive.Name) is almost full. Free up space and run the script again; it resumes."
            }
        }
        if ($task.IsFaulted) { throw $task.Exception }
    } finally {
        $fs.Dispose()
        Write-Progress -Activity $Label -Completed
    }
    $p.WaitForExit()
    $e = $err.Result.Trim()
    if ($p.ExitCode -ne 0) { throw "adb exec-out failed (exit $($p.ExitCode)): $e" }
    return $sw.Elapsed
}

# Send InFile to ShellCmd's stdin on the phone (adb exec-in).
function Send-AdbStream([string]$ShellCmd, [string]$InFile, [string]$Label) {
    $p = Start-AdbProcess "exec-in" $ShellCmd -Stdin
    $err = $p.StandardError.ReadToEndAsync()
    $out = $p.StandardOutput.ReadToEndAsync()
    $fs = [System.IO.File]::OpenRead($InFile)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try {
        $dst = $p.StandardInput.BaseStream
        $task = $fs.CopyToAsync($dst, 4MB)
        while (-not $task.Wait(1000)) {
            $pct = [int](100 * $fs.Position / [math]::Max($fs.Length, 1))
            $mbs = $fs.Position / 1MB / [math]::Max($sw.Elapsed.TotalSeconds, 1)
            Write-Progress -Activity $Label -PercentComplete $pct `
                -Status ("{0} of {1} sent, {2:N0} MB/s" -f (Format-Size $fs.Position), (Format-Size $fs.Length), $mbs)
        }
        if ($task.IsFaulted) { throw $task.Exception }
        $dst.Flush()
        $dst.Close()           # EOF for the phone side
    } finally {
        $fs.Dispose()
        Write-Progress -Activity $Label -Completed
    }
    $p.WaitForExit()
    $e = $err.Result.Trim()
    if ($p.ExitCode -ne 0) { throw "adb exec-in failed (exit $($p.ExitCode)): $e $($out.Result)" }
    return $sw.Elapsed
}

# SHA-256 of the decompressed content of a .gz file (lower-case hex).
function Get-GzipSha256([string]$Path, [string]$Label) {
    $in  = [System.IO.File]::OpenRead($Path)
    $gz  = New-Object System.IO.Compression.GZipStream($in, [System.IO.Compression.CompressionMode]::Decompress)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $buf = New-Object byte[] (4MB)
    [int64]$total = 0
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try {
        while (($n = $gz.Read($buf, 0, $buf.Length)) -gt 0) {
            $null = $sha.TransformBlock($buf, 0, $n, $null, 0)
            $total += $n
            if ($sw.ElapsedMilliseconds -gt 1000) {
                Write-Progress -Activity $Label -Status ("{0} checked" -f (Format-Size $total)) `
                    -PercentComplete ([int](100 * $in.Position / [math]::Max($in.Length, 1)))
                $sw.Restart()
            }
        }
        $null = $sha.TransformFinalBlock($buf, 0, 0)
    } finally {
        $gz.Dispose(); $in.Dispose()
        Write-Progress -Activity $Label -Completed
    }
    [pscustomobject]@{ Hash = (($sha.Hash | ForEach-Object { $_.ToString("x2") }) -join ""); Bytes = $total }
}

# Unmount everything TWRP may have mounted from the partitions we read/write.
function Dismount-PhoneFilesystems {
    $cmd = "for m in /data /sdcard /metadata /cache /system_root /system /vendor /product /odm /system_ext /prism /optics /efs; do umount `$m 2>/dev/null; done; true"
    Invoke-PhoneShell $cmd | Out-Null
    # Return whatever is still mounted from the UFS (TWRP sometimes holds /data).
    $left = Invoke-PhoneShell "grep -E ' /(data|sdcard|metadata|cache|system_root|vendor|product|odm)( |/)' /proc/mounts | cut -d' ' -f2"
    return @($left -split "`n" | Where-Object { $_ })
}

# Name -> node / size of every partition the phone exposes by name.
function Get-PhonePartitions {
    $lines = Invoke-PhoneShell "cd /dev/block/by-name && for p in *; do n=`$(readlink -f `$p); s=`$(blockdev --getsize64 `$n 2>/dev/null); echo `$p `$n `$s; done"
    foreach ($l in ($lines -split "`n")) {
        $f = $l.Trim() -split "\s+"
        if ($f.Count -lt 3 -or $f[2] -notmatch '^\d+$') { continue }
        [pscustomobject]@{ Name = $f[0]; Node = $f[1]; Size = [int64]$f[2] }
    }
}

# Parse a GPT header (sector 1) out of the first sectors of a LUN.
function Get-GptLayout([byte[]]$Head, [int]$Ss) {
    $sig = [Text.Encoding]::ASCII.GetString($Head, $Ss, 8)
    if ($sig -ne "EFI PART") { return $null }
    $entLba   = [BitConverter]::ToUInt64($Head, $Ss + 72)
    $entCount = [BitConverter]::ToUInt32($Head, $Ss + 80)
    $entSize  = [BitConverter]::ToUInt32($Head, $Ss + 84)
    $entBlocks = [int][math]::Ceiling($entCount * $entSize / $Ss)
    [pscustomobject]@{
        AltLba     = [BitConverter]::ToUInt64($Head, $Ss + 32)
        EntLba     = $entLba
        EntBlocks  = $entBlocks
        PrimarySectors = [int]($entLba + $entBlocks)   # LBA 0 .. end of primary entries
        BackupSectors  = $entBlocks + 1                # backup entries + backup header at the end
    }
}

# Check an r8s-sparse backup file (see src/r8s-sparse.c) on the PC: every
# chunk's CRC, the record order, and the digest in its trailer.
if (-not ("R8sSparse" -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.IO;
public class R8sSparseResult { public string Digest; public long Chunks; public long NonZero; public long Total; }
public static class R8sSparse {
    static uint[] T;
    static uint Crc(byte[] b, int n) {
        if (T == null) {
            T = new uint[256];
            for (uint i = 0; i < 256; i++) { uint c = i; for (int k = 0; k < 8; k++) c = ((c & 1) != 0) ? 0xEDB88320u ^ (c >> 1) : c >> 1; T[i] = c; }
        }
        uint x = 0xFFFFFFFFu;
        for (int i = 0; i < n; i++) x = T[(x ^ b[i]) & 255] ^ (x >> 8);
        return x ^ 0xFFFFFFFFu;
    }
    static ulong Fnv(ulong h, ulong v, int bytes) {
        for (int i = 0; i < bytes; i++) { h ^= (v >> (8 * i)) & 255UL; h *= 0x100000001b3UL; }
        return h;
    }
    static void Fill(Stream s, byte[] b, int n) {
        int o = 0;
        while (o < n) { int r = s.Read(b, o, n - o); if (r <= 0) throw new Exception("file ends early (backup incomplete)"); o += r; }
    }
    public static R8sSparseResult Verify(string path) {
        using (FileStream s = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 22)) {
            byte[] h = new byte[32]; Fill(s, h, 32);
            if (System.Text.Encoding.ASCII.GetString(h, 0, 8) != "R8SSPRS1") throw new Exception("not an r8s-sparse file");
            ulong total = BitConverter.ToUInt64(h, 8);
            uint chunk = BitConverter.ToUInt32(h, 16);
            if (chunk == 0 || chunk > (1u << 24)) throw new Exception("bad chunk size");
            ulong d = Fnv(0xcbf29ce484222325UL, total, 8), next = 0;
            long n = 0, nz = 0;
            byte[] rec = new byte[16], buf = new byte[chunk];
            while (true) {
                Fill(s, rec, 16);
                ulong off = BitConverter.ToUInt64(rec, 0);
                uint len = BitConverter.ToUInt32(rec, 8), c = BitConverter.ToUInt32(rec, 12);
                if (off == ulong.MaxValue) break;
                if (len == 0 || len > chunk || off < next || off + len > total) throw new Exception("corrupt record at file position " + s.Position);
                Fill(s, buf, (int)len);
                if (Crc(buf, (int)len) != c) throw new Exception("checksum mismatch in the chunk at offset " + off);
                d = Fnv(Fnv(d, off, 8), c, 4); n++; nz += len; next = off + len;
            }
            byte[] tr = new byte[24]; Fill(s, tr, 24);
            if (BitConverter.ToInt64(tr, 0) != n || BitConverter.ToUInt64(tr, 8) != d) throw new Exception("trailer doesn't match the records");
            R8sSparseResult r = new R8sSparseResult();
            r.Digest = d.ToString("x16"); r.Chunks = n; r.NonZero = nz; r.Total = (long)total;
            return r;
        }
    }
}
'@
}

# Copy the sparse helper to TWRP's /tmp.
function Install-SparseHelper {
    $helper = Join-Path $PSScriptRoot "bin\r8s-sparse"
    if (-not (Test-Path $helper)) { throw "Missing $helper (comes with the repo)." }
    Invoke-Adb push $helper /tmp/r8s-sparse | Out-Null
    $v = Invoke-PhoneShell "chmod 755 /tmp/r8s-sparse; /tmp/r8s-sparse 2>&1; true"
    if ($v -notmatch "usage: r8s-sparse") { throw "The helper doesn't run in TWRP: $v" }
}
