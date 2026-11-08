function Get-ReleaseInfo([string]$ProjectRoot, [string]$BuildDirectory = 'build') {
    $root = [IO.Path]::GetFullPath($ProjectRoot)
    $buildRoot = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
        [IO.Path]::GetFullPath($BuildDirectory)
    } else { [IO.Path]::GetFullPath((Join-Path $root $BuildDirectory)) }
    $metadata = Get-Content -LiteralPath (Join-Path $buildRoot 'release-version.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($metadata.version -notmatch '^\d+\.\d+\.\d+$' -or $metadata.native_version -ne "$($metadata.version)-native") {
        throw 'Invalid CMake release metadata; configure the requested build first'
    }
    [pscustomobject]@{
        ProjectRoot = $root; BuildRoot = $buildRoot
        Version = $metadata.version; NativeVersion = $metadata.native_version
        PackageRoot = Join-Path $root "dist\MSA_Data_Generator_v$($metadata.version)_Windows_x64"
        SourceZip = Join-Path $root "dist\MSA_Data_Generator_v$($metadata.version)_Source.zip"
    }
}

function Initialize-EmbeddedResourceReader {
    if(-not ('Msa.ReleaseResources' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Security.Cryptography;

namespace Msa {
    public static class ReleaseResources {
        private const int MaximumFontBytes = 64 * 1024 * 1024;
        private static readonly byte[] FontMagic = new byte[] {77, 83, 65, 70, 78, 84, 90, 49};
        private static readonly uint[] CrcTable = CreateCrcTable();
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryExW(string file, IntPtr reserved, uint flags);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr FindResourceW(IntPtr module, string name, IntPtr type);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint SizeofResource(IntPtr module, IntPtr resource);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr LockResource(IntPtr resource);
        [DllImport("kernel32.dll")]
        private static extern bool FreeLibrary(IntPtr module);

        private static byte[] ReadResource(string executable, string name, int maximumSize) {
            // 仅映射资源，不执行入口、导入或 DLL 初始化代码。
            IntPtr module = LoadLibraryExW(executable, IntPtr.Zero, 0x00000002);
            if (module == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot map executable resources");
            try {
                IntPtr resource = FindResourceW(module, name, new IntPtr(10));
                if (resource == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "Missing embedded resource: " + name);
                uint size = SizeofResource(module, resource);
                if (size == 0 || size > maximumSize) throw new InvalidOperationException("Invalid embedded resource size");
                IntPtr loaded = LoadResource(module, resource);
                if (loaded == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot load embedded resource");
                IntPtr data = LockResource(loaded);
                if (data == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot read embedded resource");
                byte[] bytes = new byte[(int)size];
                Marshal.Copy(data, bytes, 0, bytes.Length);
                return bytes;
            } finally {
                FreeLibrary(module);
            }
        }

        private static string Hash(byte[] bytes) {
            using (SHA256 sha = SHA256.Create()) {
                return BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "");
            }
        }

        public static string Sha256(string executable, string name) {
            return Hash(ReadResource(executable, name, Int32.MaxValue));
        }

        private static uint ReadUnsigned(byte[] data, ref int offset) {
            if (offset < 0 || data.Length - offset < 4)
                throw new InvalidDataException("字体容器整数被截断");
            uint value = (uint)data[offset] | ((uint)data[offset + 1] << 8) |
                         ((uint)data[offset + 2] << 16) | ((uint)data[offset + 3] << 24);
            offset += 4;
            return value;
        }

        private static uint[] CreateCrcTable() {
            uint[] table = new uint[256];
            for (uint index = 0; index < 256; ++index) {
                uint value = index;
                for (int bit = 0; bit < 8; ++bit)
                    value = (value & 1) != 0 ? 0xedb88320U ^ (value >> 1) : value >> 1;
                table[index] = value;
            }
            return table;
        }

        private static uint Crc32(byte[] data) {
            uint crc = 0xffffffffU;
            for (int index = 0; index < data.Length; ++index)
                crc = CrcTable[(crc ^ data[index]) & 255] ^ (crc >> 8);
            return crc ^ 0xffffffffU;
        }

        private static uint Adler32(byte[] data, int offset, int length) {
            uint a = 1, b = 0;
            while (length > 0) {
                int count = Math.Min(length, 5552);
                length -= count;
                while (count-- > 0) {
                    a += data[offset++];
                    b += a;
                }
                a %= 65521;
                b %= 65521;
            }
            return (b << 16) | a;
        }

        public static byte[] DecodeFont(byte[] packed) {
            if (packed == null || packed.Length < 28)
                throw new InvalidDataException("压缩字体容器缺失或被截断");
            for (int index = 0; index < FontMagic.Length; ++index)
                if (packed[index] != FontMagic[index])
                    throw new InvalidDataException("字体资源不是 MSAFNTZ1 压缩容器");
            int cursor = FontMagic.Length;
            uint total = ReadUnsigned(packed, ref cursor);
            uint count = ReadUnsigned(packed, ref cursor);
            uint expectedCrc = ReadUnsigned(packed, ref cursor);
            if (total == 0 || total > MaximumFontBytes || count == 0 || count > 32 || count > total)
                throw new InvalidDataException("压缩字体长度或分块数量无效");
            if (packed.Length - cursor < count * 8)
                throw new InvalidDataException("压缩字体块表被截断");
            int[] originalSizes = new int[(int)count];
            int[] compressedSizes = new int[(int)count];
            long originalSum = 0, compressedSum = 0;
            for (int index = 0; index < count; ++index) {
                uint original = ReadUnsigned(packed, ref cursor);
                uint compressed = ReadUnsigned(packed, ref cursor);
                if (original == 0 || original > total || compressed < 6 || compressed > packed.Length)
                    throw new InvalidDataException("压缩字体块长度无效");
                originalSizes[index] = (int)original;
                compressedSizes[index] = (int)compressed;
                originalSum += original;
                compressedSum += compressed;
            }
            if (originalSum != total || compressedSum != packed.Length - cursor)
                throw new InvalidDataException("字体容器长度不匹配或含有尾随数据");
            byte[] restored = new byte[(int)total];
            int destination = 0;
            for (int index = 0; index < count; ++index) {
                int compressed = compressedSizes[index];
                int original = originalSizes[index];
                int header = (packed[cursor] << 8) | packed[cursor + 1];
                if ((packed[cursor] & 15) != 8 || (packed[cursor] >> 4) > 7 ||
                    header % 31 != 0 || (packed[cursor + 1] & 32) != 0)
                    throw new InvalidDataException("字体块 zlib 头无效");
                // Windows PowerShell 5.1 的 DeflateStream 读取原始 Deflate，须剥离 zlib 包装。
                using (MemoryStream input = new MemoryStream(packed, cursor + 2, compressed - 6, false))
                using (DeflateStream decoder = new DeflateStream(input, CompressionMode.Decompress)) {
                    int written = 0;
                    while (written < original) {
                        int read = decoder.Read(restored, destination + written, original - written);
                        if (read == 0) break;
                        written += read;
                    }
                    if (written != original || decoder.ReadByte() != -1)
                        throw new InvalidDataException("字体块解压长度不匹配");
                }
                int tail = cursor + compressed - 4;
                uint expectedAdler = ((uint)packed[tail] << 24) | ((uint)packed[tail + 1] << 16) |
                                     ((uint)packed[tail + 2] << 8) | packed[tail + 3];
                if (Adler32(restored, destination, original) != expectedAdler)
                    throw new InvalidDataException("字体块 zlib 校验和不匹配");
                destination += original;
                cursor += compressed;
            }
            if (cursor != packed.Length || Crc32(restored) != expectedCrc)
                throw new InvalidDataException("完整字体 CRC32 不匹配");
            return restored;
        }

        public static string FontSha256(string executable, string name) {
            return Hash(DecodeFont(ReadResource(executable, name, MaximumFontBytes + 1024 * 1024)));
        }
    }
}
'@
    }
}

function Get-EmbeddedResourceSha256([string]$ExecutablePath, [string]$ResourceName = 'MSA_FONT_NOTO_CJK') {
    Initialize-EmbeddedResourceReader
    [Msa.ReleaseResources]::Sha256([IO.Path]::GetFullPath($ExecutablePath), $ResourceName)
}

function Get-EmbeddedFontSha256([string]$ExecutablePath, [string]$ResourceName = 'MSA_FONT_NOTO_CJK') {
    Initialize-EmbeddedResourceReader
    [Msa.ReleaseResources]::FontSha256([IO.Path]::GetFullPath($ExecutablePath), $ResourceName)
}

function Assert-EmbeddedFont([string]$ExecutablePath, [string]$ExpectedSha256) {
    if($ExpectedSha256 -notmatch '^[a-fA-F0-9]{64}$') { throw 'Invalid expected embedded font SHA256' }
    $actual = Get-EmbeddedFontSha256 $ExecutablePath
    if($actual -ne $ExpectedSha256) { throw 'Embedded font differs from locked source' }
    $actual
}

function Get-ProjectLicenseFiles {
    [ordered]@{'LICENSE'='LICENSE';'NOTICE'='NOTICE';'licenses/MSA-Project-LICENSE.txt'='LICENSE';'licenses/MSA-Project-NOTICE.txt'='NOTICE'}
}

function Copy-ProjectLicense([string]$PackageRoot, [string]$ProjectRoot) {
    New-Item -ItemType Directory -Force -Path (Join-Path $PackageRoot 'licenses') | Out-Null
    foreach($entry in (Get-ProjectLicenseFiles).GetEnumerator()) {
        Copy-Item -LiteralPath (Join-Path $ProjectRoot $entry.Value) -Destination (Join-Path $PackageRoot $entry.Key) -Force
    }
}

function Assert-ProjectLicense([string]$PackageRoot, [string]$ProjectRoot) {
    foreach($entry in (Get-ProjectLicenseFiles).GetEnumerator()) {
        $packaged=Join-Path $PackageRoot $entry.Key
        $source=Join-Path $ProjectRoot $entry.Value
        if(-not (Test-Path -LiteralPath $packaged -PathType Leaf)) { throw "Missing project legal file: $($entry.Key)" }
        if((Get-FileHash -LiteralPath $packaged -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash) {
            throw "Project legal file differs from source: $($entry.Key)"
        }
    }
}

function Assert-EvidenceBinding($Report, [hashtable]$Artifacts, [string]$NativeReportPath, [string]$NativeVersion) {
    if ($Report.passed -ne $true -or $Report.version -ne $NativeVersion) {
        throw 'Native evidence is missing, failed, or from another version'
    }
    foreach ($name in $Artifacts.Keys) {
        $property = $Report.artifacts.PSObject.Properties[$name]
        if (-not $property -or $property.Value -ne $Artifacts[$name]) {
            throw "Native evidence SHA256 mismatch: $name"
        }
    }
    $nativeHash = (Get-FileHash -LiteralPath $NativeReportPath -Algorithm SHA256).Hash
    if ($Report.native_report_sha256 -ne $nativeHash) {
        throw 'Native regression report SHA256 mismatch'
    }
}
