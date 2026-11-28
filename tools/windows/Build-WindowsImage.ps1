<#
.SYNOPSIS
  Build full Windows 11 for the phone on the PC: Administrator PowerShell, phone
  in TWRP (only read from: partition sizes and GUIDs). Then Flash-Windows.ps1.

.DESCRIPTION
  The method of the Galaxy S9+ project's installer (VhdxImageBuilder,
  BootConfiguration), which installs Windows 11 on an Exynos phone:

   1. reads the phone's GPT: the size of "win", the disk GUID, and the unique
      GUIDs of "esp" and "win";
   2. creates a dynamic VHDX with 4096-byte sectors (the UFS is a 4Kn device:
      NTFS made for 512-byte sectors will not mount there) holding an ESP and
      an NTFS partition exactly as big as "win";
   3. applies Windows from the ISO (install.wim/esd, Pro by default);
   4. adds the r8s drivers (D:\woa\drivers: UFS, touch, log) with DISM;
   5. writes an answer file: no setup questions, a local administrator account
      "User" (password r8s) that signs in by itself;
   6. bcdboot, then points the BCD at the PHONE's partitions (boot manager on
      "esp", Windows on "win", by GUID), with test signing on, no hypervisor
      or VSM, no boot menu, ignoring boot-status failures;
   7. saves the NTFS partition as D:\woa\out\windows\win.sparse (only the
      non-empty 1 MiB blocks, each with a CRC; the format r8s-sparse restores)
      and the boot files as D:\woa\out\windows\esp\.

  Takes a while: applying Windows ~15-30 min, saving the image ~10 min.
#>
param(
    [string]$Iso,
    [string]$Edition = "Windows 11 Pro",
    [string]$Drivers = "D:\woa\drivers",
    [string]$Out     = "D:\woa\out\windows",
    [string]$Scratch = "D:\woa\scratch",
    [string]$UserName = "User",
    [string]$Password = "r8s",
    # Keep the VHDX of an earlier run that got as far as the drivers (skips the 15-30 min apply).
    [switch]$Reuse
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\..\device\Common.ps1"

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this in an Administrator PowerShell."
}
foreach ($p in $Out, $Scratch) {
    if ($p -match '^[Cc]:') { throw "Refusing to work on C: ($p). Use D:\woa." }
    New-Item -ItemType Directory -Force -Path $p | Out-Null
}
if (-not $Iso) {
    $Iso = Get-ChildItem "D:\woa\iso" -Recurse -Filter *.iso -ErrorAction SilentlyContinue |
        Where-Object Name -match 'A64|ARM64' | Sort-Object Length -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Iso -or -not (Test-Path $Iso)) { throw "No ARM64 Windows ISO found in D:\woa\iso. Pass -Iso." }
if (-not (Test-Path (Join-Path $Drivers "r8sufs\r8sufs.inf"))) { throw "$Drivers has no r8sufs driver: unzip r8s-drivers-arm64 there." }
Write-Host "ISO: $Iso"

# ---------------------------------------------------------------- 1. the phone's GPT
$bl = Assert-R8sInRecovery
$win = Get-PartitionNode "win"
$esp = Get-PartitionNode "esp"
$lun = ($win -replace '^/dev/block/(sd[a-z])\d+$', '$1')
if ($lun -eq $win) { throw "Unexpected node for win: $win" }
$winBytes = [int64](Invoke-PhoneShell "blockdev --getsize64 $win")
$ss = [int](Invoke-PhoneShell "blockdev --getss /dev/block/$lun")
if ($ss -ne 4096) { throw "The phone's UFS has $ss-byte sectors, expected 4096." }

$tmp = Join-Path $Scratch "gpt-head.bin"
Invoke-PhoneShell "dd if=/dev/block/$lun of=/tmp/r8s_gpt.bin bs=4096 count=64 2>/dev/null" | Out-Null
Invoke-Adb pull /tmp/r8s_gpt.bin $tmp | Out-Null
Invoke-PhoneShell "rm -f /tmp/r8s_gpt.bin" | Out-Null
$g = [IO.File]::ReadAllBytes($tmp)
if ([Text.Encoding]::ASCII.GetString($g, $ss, 8) -ne "EFI PART") { throw "No GPT on $lun." }
$diskGuid = [Guid]::new([byte[]]$g[($ss + 56)..($ss + 71)])
$entLba = [BitConverter]::ToUInt64($g, $ss + 72)
$entCount = [BitConverter]::ToUInt32($g, $ss + 80)
$entSize = [BitConverter]::ToUInt32($g, $ss + 84)
$guids = @{}
for ($i = 0; $i -lt $entCount; $i++) {
    $o = [int]($entLba * $ss + $i * $entSize)
    if ($o + 128 -gt $g.Length) { break }
    $name = [Text.Encoding]::Unicode.GetString($g, $o + 56, 72).TrimEnd([char]0)
    if ($name -eq "esp" -or $name -eq "win") { $guids[$name] = [Guid]::new([byte[]]$g[($o + 16)..($o + 31)]) }
}
if (-not $guids["esp"] -or -not $guids["win"]) { throw "esp/win not found in the GPT of $lun. Run Partition-Phone.ps1 first." }
Write-Host ("Phone: {0}, win {1:N1} GB, disk {2}, esp {3}, win {4}" -f $bl, ($winBytes / 1GB), $diskGuid, $guids["esp"], $guids["win"])

# ---------------------------------------------------------------- helpers
if (-not ("R8sReg" -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class R8sVhd {
    [StructLayout(LayoutKind.Sequential)]
    public struct VIRTUAL_STORAGE_TYPE { public uint DeviceId; public Guid VendorId; }

    [StructLayout(LayoutKind.Sequential)]
    public struct CREATE_PARAMS_V2 {
        public int Version; public int Pad0;
        public Guid UniqueId; public ulong MaximumSize; public uint BlockSizeInBytes;
        public uint SectorSizeInBytes; public uint PhysicalSectorSizeInBytes;
        public IntPtr ParentPath; public IntPtr SourcePath; public int OpenFlags;
        public VIRTUAL_STORAGE_TYPE ParentVirtualStorageType; public VIRTUAL_STORAGE_TYPE SourceVirtualStorageType;
        public Guid ResiliencyGuid;
    }

    [DllImport("virtdisk.dll", CharSet = CharSet.Unicode)]
    static extern int CreateVirtualDisk(ref VIRTUAL_STORAGE_TYPE type, string path, int access, IntPtr sd, int flags,
                                        int providerFlags, ref CREATE_PARAMS_V2 p, IntPtr overlapped, out IntPtr handle);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

    /* Dynamic VHDX with 4096-byte logical and physical sectors (no Hyper-V module needed). */
    public static void Create4Kn(string path, ulong size) {
        VIRTUAL_STORAGE_TYPE t = new VIRTUAL_STORAGE_TYPE();
        t.DeviceId = 3;     /* VHDX */
        t.VendorId = new Guid("EC984AEC-A0F9-47e9-901F-71415A66345B");
        CREATE_PARAMS_V2 p = new CREATE_PARAMS_V2();
        p.Version = 2; p.UniqueId = Guid.NewGuid(); p.MaximumSize = size;
        p.SectorSizeInBytes = 4096; p.PhysicalSectorSizeInBytes = 4096;
        IntPtr h;
        int r = CreateVirtualDisk(ref t, path, 0, IntPtr.Zero, 0, 0, ref p, IntPtr.Zero, out h);
        if (r != 0) throw new Exception("CreateVirtualDisk failed: " + r);
        CloseHandle(h);
    }
}

/* Writes registry values past the key's ACL with SeRestorePrivilege (REG_OPTION_BACKUP_RESTORE). */
public static class R8sReg {
    [StructLayout(LayoutKind.Sequential)] struct LUID { public uint Low; public int High; }
    [StructLayout(LayoutKind.Sequential)] struct TOKEN_PRIVILEGES { public uint Count; public LUID Luid; public uint Attr; }
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr p, uint access, out IntPtr tok);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool LookupPrivilegeValue(string sys, string name, out LUID luid);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool AdjustTokenPrivileges(IntPtr tok, bool all, ref TOKEN_PRIVILEGES p, uint len, IntPtr prev, IntPtr retlen);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)] static extern int RegCreateKeyEx(UIntPtr root, string sub, int res, string cls, int opt, int sam, IntPtr sa, out UIntPtr key, out int disp);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)] static extern int RegSetValueEx(UIntPtr key, string name, int res, int type, byte[] data, int len);
    [DllImport("advapi32.dll")] static extern int RegCloseKey(UIntPtr key);
    static readonly UIntPtr HKLM = new UIntPtr(0x80000002u);

    static void Enable(string name) {
        IntPtr tok;
        if (!OpenProcessToken(GetCurrentProcess(), 0x28, out tok)) throw new Exception("OpenProcessToken: " + Marshal.GetLastWin32Error());
        try {
            TOKEN_PRIVILEGES tp = new TOKEN_PRIVILEGES(); tp.Count = 1; tp.Attr = 2;
            if (!LookupPrivilegeValue(null, name, out tp.Luid)) throw new Exception("LookupPrivilegeValue: " + Marshal.GetLastWin32Error());
            if (!AdjustTokenPrivileges(tok, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero) || Marshal.GetLastWin32Error() != 0)
                throw new Exception("Cannot enable " + name + ": " + Marshal.GetLastWin32Error());
        } finally { CloseHandle(tok); }
    }

    public static void MarkSystemStore(string hive) {
        Enable("SeRestorePrivilege"); Enable("SeBackupPrivilege");
        UIntPtr k; int disp;
        int r = RegCreateKeyEx(HKLM, hive + "\\Description", 0, null, 4 /* BACKUP_RESTORE */, 0x2 | 0x4 /* SET_VALUE|CREATE_SUB_KEY */, IntPtr.Zero, out k, out disp);
        if (r != 0) throw new Exception("Opening " + hive + "\\Description failed: " + r);
        try {
            byte[] name = System.Text.Encoding.Unicode.GetBytes("BCD00000000\0");
            Set(k, "KeyName", 1, name);
            Set(k, "System", 4, BitConverter.GetBytes(1));
            Set(k, "TreatAsSystem", 4, BitConverter.GetBytes(1));
        } finally { RegCloseKey(k); }
    }
    static void Set(UIntPtr k, string n, int type, byte[] d) {
        int r = RegSetValueEx(k, n, 0, type, d, d.Length);
        if (r != 0) throw new Exception("Setting " + n + " failed: " + r);
    }
}

/* Writes the r8s-sparse format (tools/device/src/r8s-sparse.c) from a raw disk region. */
public class R8sSparseWriter : IDisposable {
    const int Chunk = 1 << 20;
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);

    FileStream src, dst; long offset, total, pos; byte[] buf = new byte[Chunk];
    static uint[] T;
    public long Chunks, NonZero; ulong digest;

    public R8sSparseWriter(string device, long offset, long total, string outPath) {
        SafeFileHandle h = CreateFile(device, 0x80000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (h.IsInvalid) throw new Exception("Cannot open " + device + ": " + Marshal.GetLastWin32Error());
        src = new FileStream(h, FileAccess.Read, Chunk);
        dst = new FileStream(outPath, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 22);
        this.offset = offset; this.total = total;
        if (T == null) {
            T = new uint[256];
            for (uint i = 0; i < 256; i++) { uint c = i; for (int k = 0; k < 8; k++) c = ((c & 1) != 0) ? 0xEDB88320u ^ (c >> 1) : c >> 1; T[i] = c; }
        }
        byte[] hdr = new byte[32];
        System.Text.Encoding.ASCII.GetBytes("R8SSPRS1").CopyTo(hdr, 0);
        BitConverter.GetBytes((ulong)total).CopyTo(hdr, 8);
        BitConverter.GetBytes((uint)Chunk).CopyTo(hdr, 16);
        dst.Write(hdr, 0, 32);
        digest = Fnv(0xcbf29ce484222325UL, (ulong)total, 8);
    }
    static ulong Fnv(ulong h, ulong v, int bytes) { for (int i = 0; i < bytes; i++) { h ^= (v >> (8 * i)) & 255UL; h *= 0x100000001b3UL; } return h; }
    static uint Crc(byte[] b, int n) { uint x = 0xFFFFFFFFu; for (int i = 0; i < n; i++) x = T[(x ^ b[i]) & 255] ^ (x >> 8); return x ^ 0xFFFFFFFFu; }

    public long Position { get { return pos; } }
    public bool Done { get { return pos >= total; } }

    /* Process up to n chunks; false when finished. */
    public bool Step(int n) {
        for (int k = 0; k < n && pos < total; k++) {
            int len = (int)Math.Min(Chunk, total - pos);
            src.Position = offset + pos;
            int got = 0;
            while (got < len) { int r = src.Read(buf, got, len - got); if (r <= 0) throw new Exception("short read at " + (offset + pos + got)); got += r; }
            bool zero = true;
            for (int i = 0; i < len; i += 8) { if (BitConverter.ToUInt64(buf, i) != 0) { zero = false; break; } }
            if (!zero) {
                uint c = Crc(buf, len);
                dst.Write(BitConverter.GetBytes((ulong)pos), 0, 8);
                dst.Write(BitConverter.GetBytes((uint)len), 0, 4);
                dst.Write(BitConverter.GetBytes(c), 0, 4);
                dst.Write(buf, 0, len);
                digest = Fnv(Fnv(digest, (ulong)pos, 8), c, 4);
                Chunks++; NonZero += len;
            }
            pos += len;
        }
        return pos < total;
    }
    public string Finish() {
        byte[] tr = new byte[40];
        BitConverter.GetBytes(ulong.MaxValue).CopyTo(tr, 0);
        BitConverter.GetBytes((ulong)Chunks).CopyTo(tr, 16);
        BitConverter.GetBytes(digest).CopyTo(tr, 24);
        BitConverter.GetBytes((ulong)NonZero).CopyTo(tr, 32);
        dst.Write(tr, 0, 40);
        dst.Flush();
        return digest.ToString("x16");
    }
    public void Dispose() { if (src != null) src.Dispose(); if (dst != null) dst.Dispose(); }
}
'@
}

$vhd = Join-Path $Scratch "windows-r8s.vhdx"
$espMB = 260
$vhdBytes = [uint64]($winBytes + ($espMB + 64) * 1MB)

function Dismount-Work {
    if (Test-Path $vhd) { Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null }
}

# ---------------------------------------------------------------- 2. the 4Kn disk
Dismount-Work
$reused = $false
if ($Reuse -and (Test-Path $vhd)) {
    $disk = Mount-DiskImage -ImagePath $vhd -NoDriveLetter -PassThru | Get-Disk
    $parts = Get-Partition -DiskNumber $disk.Number
    $espPart = $parts | Where-Object GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' | Select-Object -First 1
    $winPart = $parts | Where-Object GptType -eq '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}' | Select-Object -First 1
    if ($espPart -and $winPart -and $winPart.Size -eq $winBytes) {
        $used = (Get-PSDrive -PSProvider FileSystem).Name
        $free = [char[]]"WVUTSRQPONMLKJIH" | Where-Object { $used -notcontains [string]$_ }
        $W = [string]$free[0]; $E = [string]$free[1]
        Set-Partition -DiskNumber $disk.Number -PartitionNumber $winPart.PartitionNumber -NewDriveLetter $W
        Set-Partition -DiskNumber $disk.Number -PartitionNumber $espPart.PartitionNumber -NewDriveLetter $E
        $winOffset = $winPart.Offset
        if (Test-Path "${W}:\Windows\System32\DriverStore\FileRepository\r8sufs.inf_*") {
            $reused = $true
            Write-Host "==> Reusing $vhd (Windows and drivers already in it)"
        }
    }
    if (-not $reused) {
        Write-Host "    $vhd is not a finished earlier run: starting over"
        Dismount-Work
    }
}
if (-not $reused) {
if (Test-Path $vhd) { Remove-Item $vhd -Force }
Write-Host "==> 4Kn VHDX $vhd ($([math]::Round($vhdBytes / 1GB, 1)) GB, dynamic)"
[R8sVhd]::Create4Kn($vhd, $vhdBytes)
$disk = Mount-DiskImage -ImagePath $vhd -NoDriveLetter -PassThru | Get-Disk
if ($disk.LogicalSectorSize -ne 4096) { throw "The VHDX is not 4Kn ($($disk.LogicalSectorSize))." }
Initialize-Disk -Number $disk.Number -PartitionStyle GPT
$espPart = New-Partition -DiskNumber $disk.Number -Size ($espMB * 1MB) -GptType '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
$winPart = New-Partition -DiskNumber $disk.Number -Size $winBytes -Alignment 4096 -GptType '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'
if ($winPart.Size -ne $winBytes) { throw "Windows partition is $($winPart.Size) bytes, not $winBytes." }
Format-Volume -Partition $espPart -FileSystem FAT32 -NewFileSystemLabel "ESP" -Confirm:$false | Out-Null
Format-Volume -Partition $winPart -FileSystem NTFS -AllocationUnitSize 4096 -NewFileSystemLabel "Windows" -Confirm:$false | Out-Null
$used = (Get-PSDrive -PSProvider FileSystem).Name
$free = [char[]]"WVUTSRQPONMLKJIH" | Where-Object { $used -notcontains [string]$_ }
$W = [string]$free[0]; $E = [string]$free[1]
Set-Partition -DiskNumber $disk.Number -PartitionNumber $winPart.PartitionNumber -NewDriveLetter $W
Set-Partition -DiskNumber $disk.Number -PartitionNumber $espPart.PartitionNumber -NewDriveLetter $E
$winOffset = (Get-Partition -DiskNumber $disk.Number -PartitionNumber $winPart.PartitionNumber).Offset
}

try {
    if (-not $reused) {
    # ------------------------------------------------------------ 3. Windows
    $img = Mount-DiskImage -ImagePath $Iso -PassThru
    try {
        $isoLetter = ($img | Get-Volume).DriveLetter
        $wim = "${isoLetter}:\sources\install.wim"
        if (-not (Test-Path $wim)) { $wim = "${isoLetter}:\sources\install.esd" }
        $images = Get-WindowsImage -ImagePath $wim
        $pick = $images | Where-Object ImageName -eq $Edition | Select-Object -First 1
        if (-not $pick) {
            Write-Host ($images | Format-Table ImageIndex, ImageName -AutoSize | Out-String)
            throw "Edition '$Edition' is not in $wim. Pass -Edition with one of the names above."
        }
        Write-Host "==> Applying $($pick.ImageName) (index $($pick.ImageIndex)) to ${W}: (15-30 min)"
        Expand-WindowsImage -ImagePath $wim -Index $pick.ImageIndex -ApplyPath "${W}:\" -ScratchDirectory $Scratch | Out-Null
    } finally {
        Dismount-DiskImage -ImagePath $Iso | Out-Null
    }

    # ------------------------------------------------------------ 4. drivers
    Write-Host "==> Adding drivers from $Drivers"
    Add-WindowsDriver -Path "${W}:\" -Driver $Drivers -Recurse -ForceUnsigned -ScratchDirectory $Scratch |
        ForEach-Object { "    " + $_.OriginalFileName }
    }

    # ------------------------------------------------------------ 5. answer file
    Write-Host "==> Answer file (local account '$UserName', password '$Password', signs in by itself)"
    $u = [Security.SecurityElement]::Escape($UserName); $pw = [Security.SecurityElement]::Escape($Password)
    $unattend = @"
<?xml version="1.0" encoding="utf-8"?>
<unattend xmlns="urn:schemas-microsoft-com:unattend" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State">
  <settings pass="specialize">
    <component name="Microsoft-Windows-Shell-Setup" processorArchitecture="arm64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
      <ComputerName>R8S-WOA</ComputerName>
    </component>
  </settings>
  <settings pass="oobeSystem">
    <component name="Microsoft-Windows-International-Core" processorArchitecture="arm64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
      <InputLocale>en-US</InputLocale>
      <SystemLocale>en-US</SystemLocale>
      <UILanguage>en-US</UILanguage>
      <UserLocale>en-US</UserLocale>
    </component>
    <component name="Microsoft-Windows-Shell-Setup" processorArchitecture="arm64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
      <OOBE>
        <HideEULAPage>true</HideEULAPage>
        <HideOEMRegistrationScreen>true</HideOEMRegistrationScreen>
        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>
        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>
        <HideLocalAccountScreen>true</HideLocalAccountScreen>
        <ProtectYourPC>3</ProtectYourPC>
      </OOBE>
      <UserAccounts>
        <LocalAccounts>
          <LocalAccount wcm:action="add">
            <Name>$u</Name>
            <Group>Administrators</Group>
            <Password><Value>$pw</Value><PlainText>true</PlainText></Password>
          </LocalAccount>
        </LocalAccounts>
      </UserAccounts>
      <AutoLogon>
        <Enabled>true</Enabled>
        <Username>$u</Username>
        <Password><Value>$pw</Value><PlainText>true</PlainText></Password>
        <LogonCount>9999</LogonCount>
      </AutoLogon>
      <TimeZone>Arab Standard Time</TimeZone>
    </component>
  </settings>
</unattend>
"@
    New-Item -ItemType Directory -Force -Path "${W}:\Windows\Panther" | Out-Null
    [IO.File]::WriteAllText("${W}:\Windows\Panther\unattend.xml", $unattend, (New-Object Text.UTF8Encoding $false))

    # ------------------------------------------------------------ 6. boot files, BCD at the phone
    Write-Host "==> bcdboot, then the BCD points at the phone's esp/win"
    $store = "${E}:\EFI\Microsoft\Boot\BCD"
    $o = bcdboot "${W}:\Windows" /s "${E}:" /f UEFI 2>&1
    if ($LASTEXITCODE -ne 0) {
        # The PC's (x64) bcdboot often can't copy an ARM64 Windows' boot files. Do what it does by hand:
        # boot files from the image, then a BCD with a boot manager and one Windows entry.
        Write-Host "    bcdboot: $o"
        Write-Host "    -> setting up the boot files and BCD by hand"
        $mb = "${E}:\EFI\Microsoft\Boot"
        if (Test-Path "${E}:\EFI") { Remove-Item "${E}:\EFI" -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $mb, "${E}:\EFI\Boot" | Out-Null
        $rc = robocopy "${W}:\Windows\Boot\EFI" $mb /E /R:1 /W:1 /NFL /NDL /NJH /NJS
        if ($LASTEXITCODE -ge 8) { throw "Copying ${W}:\Windows\Boot\EFI failed ($LASTEXITCODE): $rc" }
        $rc = robocopy "${W}:\Windows\Boot\Fonts" "$mb\Fonts" /E /R:1 /W:1 /NFL /NDL /NJH /NJS
        if ($LASTEXITCODE -ge 8) { throw "Copying the boot fonts failed ($LASTEXITCODE): $rc" }
        if (Test-Path "${W}:\Windows\Boot\Resources") {
            $rc = robocopy "${W}:\Windows\Boot\Resources" "$mb\Resources" /E /R:1 /W:1 /NFL /NDL /NJH /NJS
            if ($LASTEXITCODE -ge 8) { throw "Copying the boot resources failed ($LASTEXITCODE): $rc" }
        }
        if (-not (Test-Path "$mb\bootmgfw.efi")) { throw "No bootmgfw.efi in ${W}:\Windows\Boot\EFI." }
        Copy-Item "$mb\bootmgfw.efi" "${E}:\EFI\Boot\bootaa64.efi" -Force
        if (Test-Path $store) { Remove-Item $store -Force }
        $bcd = { $r = bcdedit /store $store @args 2>&1; if ($LASTEXITCODE -ne 0) { throw "bcdedit $($args -join ' ') failed: $r" }; "$r" }
        $r = bcdedit /createstore $store 2>&1
        if ($LASTEXITCODE -ne 0) { throw "bcdedit /createstore failed: $r" }
        # Mark it as a system store, as bcdboot does. Without this, setup's specialize pass
        # (Sysprep_Specialize_Bcd) fails "File is not system store" and Windows setup stops with
        # "could not configure Windows to run on this computer's hardware".
        $r = reg load HKLM\R8S_BCD $store 2>&1
        if ($LASTEXITCODE -ne 0) { throw "reg load of the new BCD failed: $r" }
        try {
            # The BCD's keys only let SYSTEM write: go through the restore privilege (as backup tools do).
            [R8sReg]::MarkSystemStore("R8S_BCD")
        } finally {
            [gc]::Collect(); Start-Sleep -Seconds 1
            $null = reg unload HKLM\R8S_BCD 2>&1
        }
        $null = & $bcd /create "{bootmgr}" /d "Windows Boot Manager"
        $null = & $bcd /set "{bootmgr}" device "partition=${E}:"
        $null = & $bcd /set "{bootmgr}" path "\EFI\Microsoft\Boot\bootmgfw.efi"
        $null = & $bcd /set "{bootmgr}" locale en-US
        $created = & $bcd /create /d "Windows 11" /application osloader
        if ($created -notmatch '\{[0-9a-fA-F-]{36}\}') { throw "bcdedit did not return the new entry: $created" }
        $os = $Matches[0]
        foreach ($a in @(
                @("device", "partition=${W}:"), @("osdevice", "partition=${W}:"),
                @("path", "\Windows\system32\winload.efi"), @("systemroot", "\Windows"),
                @("locale", "en-US"))) {
            $null = & $bcd /set $os @a
        }
        $null = & $bcd /set "{bootmgr}" default $os
        $null = & $bcd /set "{bootmgr}" displayorder $os
    }
    foreach ($a in @(
            @("/set", "{default}", "testsigning", "on"),
            @("/set", "{default}", "nointegritychecks", "on"),
            @("/set", "{default}", "hypervisorlaunchtype", "off"),
            @("/set", "{default}", "vsmlaunchtype", "off"),
            @("/set", "{default}", "recoveryenabled", "no"),
            @("/set", "{default}", "bootstatuspolicy", "IgnoreAllFailures"),
            @("/set", "{bootmgr}", "displaybootmenu", "no"),
            @("/timeout", "0"))) {
        $r = bcdedit /store $store @a 2>&1
        if ($LASTEXITCODE -ne 0) { throw "bcdedit $($a -join ' ') failed: $r" }
    }
    # Devices by GPT disk + partition GUID, through the BcdStore WMI provider (bcdedit can't name
    # a partition that isn't on this PC). As in the S9+ installer's BootConfiguration.
    $call = {
        param($obj, [string]$m, [hashtable]$a)
        $r = Invoke-CimMethod -InputObject $obj -MethodName $m -Arguments $a
        if (-not $r.ReturnValue) { throw "BCD rejected $m" }
        $r
    }
    $open = Invoke-CimMethod -Namespace root\WMI -ClassName BcdStore -MethodName OpenStore -Arguments @{ File = $store }
    if (-not $open.ReturnValue) { throw "Cannot open $store" }
    $bs = $open.Store
    $mgr = (& $call $bs 'OpenObject' @{ Id = '{9dea862c-5cdd-4e70-acc1-f32b344d4795}' }).Object
    $loaderId = (& $call $mgr 'GetElement' @{ Type = [uint32]0x23000003 }).Element.Id
    $loader = (& $call $bs 'OpenObject' @{ Id = $loaderId }).Object
    $null = & $call $mgr 'SetQualifiedPartitionDeviceElement' @{ Type = [uint32]0x11000001; PartitionStyle = [uint32]1; DiskSignature = $diskGuid.ToString("B"); PartitionIdentifier = $guids["esp"].ToString("B") }
    foreach ($t in [uint32]0x11000001, [uint32]0x21000001) {
        $null = & $call $loader 'SetQualifiedPartitionDeviceElement' @{ Type = $t; PartitionStyle = [uint32]1; DiskSignature = $diskGuid.ToString("B"); PartitionIdentifier = $guids["win"].ToString("B") }
    }
    bcdedit /store $store /enum all | Set-Content (Join-Path $Out "bcd.txt")

    Write-Host "==> Saving the boot files to $Out\esp"
    $espOut = Join-Path $Out "esp"
    if (Test-Path $espOut) { Remove-Item $espOut -Recurse -Force }
    $rc = robocopy "${E}:\" $espOut /E /COPY:D /A-:SHR /R:1 /W:1 /NFL /NDL /NJH /NJS
    if ($LASTEXITCODE -ge 8) { throw "robocopy of the ESP failed ($LASTEXITCODE): $rc" }
    Get-ChildItem $espOut -Recurse -Force | ForEach-Object { $_.Attributes = 'Normal' }
} catch {
    Dismount-Work
    throw
}

# ---------------------------------------------------------------- 7. the Windows partition, sparse
Write-Host "==> Saving the Windows partition (only blocks that hold data)"
Dismount-Work
$disk = Mount-DiskImage -ImagePath $vhd -Access ReadOnly -NoDriveLetter -PassThru | Get-Disk
$sparse = Join-Path $Out "win.sparse"
$wr = New-Object R8sSparseWriter ("\\.\PhysicalDrive$($disk.Number)"), $winOffset, $winBytes, $sparse
try {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($wr.Step(256)) {
        Write-Progress -Activity "Saving win.sparse" -PercentComplete ([int](100 * $wr.Position / $winBytes)) `
            -Status ("{0:N1} of {1:N1} GB scanned, {2:N1} GB of data" -f ($wr.Position / 1GB), ($winBytes / 1GB), ($wr.NonZero / 1GB))
    }
    $digest = $wr.Finish()
    Write-Progress -Activity "Saving win.sparse" -Completed
} finally {
    $wr.Dispose()
    Dismount-Work
}

[pscustomobject]@{
    Created      = (Get-Date).ToString("s")
    Iso          = $Iso
    Edition      = $Edition
    Bootloader   = $bl
    WinBytes     = $winBytes
    DiskGuid     = $diskGuid.ToString()
    EspGuid      = $guids["esp"].ToString()
    WinGuid      = $guids["win"].ToString()
    SparseFile   = "win.sparse"
    SparseDigest = $digest
    DataBytes    = $wr.NonZero
    UserName     = $UserName
} | ConvertTo-Json | Set-Content (Join-Path $Out "manifest.json")

Write-Host ("`nDone in {0:hh\:mm}: {1} ({2:N1} GB of data), boot files in {3}\esp" -f $sw.Elapsed, $sparse, ($wr.NonZero / 1GB), $Out)
Write-Host "Next, phone in TWRP:  ..\device\Flash-Windows.ps1"
Write-Host "(The work disk $vhd can be deleted once Windows boots.)"
