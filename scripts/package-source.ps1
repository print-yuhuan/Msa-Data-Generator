param([string]$BuildDirectory = 'build')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$projectRoot = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
. (Join-Path $PSScriptRoot 'release-common.ps1')
$info = Get-ReleaseInfo $projectRoot $BuildDirectory

# 读取工作区清单而非 HEAD 快照，保留未提交修改及未被忽略的新文件。
function Get-SourcePaths {
    $includePaths = @(
        'src', 'tests', 'examples', 'scripts', 'resources', 'licenses',
        'patches', 'third_party', '.github', 'docs',
        'CMakeLists.txt', 'LICENSE', 'NOTICE', 'README.md',
        'THIRD_PARTY_NOTICES.md', 'dependencies.lock.json',
        '.clang-format', '.gitignore', '.gitattributes'
    )
    $arguments = @('-C', $projectRoot, 'ls-files', '--cached', '--others',
                   '--exclude-standard', '-z', '--') + $includePaths
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = 'git'
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
    $startInfo.StandardErrorEncoding = [Text.Encoding]::UTF8
    $startInfo.Arguments = (($arguments | ForEach-Object {
        '"' + [regex]::Replace([regex]::Replace($_, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
    }) -join ' ')
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) { throw '无法启动 Git 以收集源码文件' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            throw '读取源码文件清单超时'
        }
        $listed = $stdout.GetAwaiter().GetResult()
        $gitError = $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) { throw "源码文件清单读取失败：$gitError" }
    } finally {
        $process.Dispose()
    }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($relative in $listed.Split([char[]]@([char]0), [StringSplitOptions]::RemoveEmptyEntries)) {
        if (-not $seen.Add($relative)) { continue }
        if ($relative.StartsWith('docs/') -and
            [IO.Path]::GetExtension($relative) -notin '.md', '.jpg', '.png') { continue }
        # 兼容工作区中尚未提交的删除；必需根文件另行检查。
        $path = Join-Path $projectRoot $relative
        if (-not (Test-Path -LiteralPath $path)) { continue }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "源码清单包含非普通文件（不支持子模块目录）：$relative"
        }
        $relative
    }
}

function Assert-SourceFile([string]$Path) {
    $absolute = [IO.Path]::GetFullPath($Path)
    $prefix = $projectRoot + [IO.Path]::DirectorySeparatorChar
    if (-not $absolute.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "源码路径超出项目目录：$Path"
    }
    # 不跟随符号链接或目录联接，避免把工作区外的本地文件带入源码包。
    $current = $absolute
    while ($current -ne $projectRoot) {
        $item = Get-Item -LiteralPath $current -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "源码打包不支持符号链接或目录联接：$Path"
        }
        $current = [IO.Path]::GetDirectoryName($current)
    }
    if (-not (Test-Path -LiteralPath $absolute -PathType Leaf)) {
        throw "源码文件缺失：$Path"
    }
}

$requiredFiles = @('CMakeLists.txt', 'LICENSE', 'NOTICE', 'README.md',
                   'THIRD_PARTY_NOTICES.md', 'dependencies.lock.json',
                   '.clang-format', '.gitignore', '.gitattributes')
$paths = @(Get-SourcePaths)
foreach ($required in $requiredFiles) {
    if ($required -notin $paths) { throw "源码清单缺少必需文件：$required" }
}
# 以归档名去重；许可专用目录的副本始终使用根目录许可证正文。
$entries = [Collections.Generic.SortedDictionary[string,string]]::new([StringComparer]::Ordinal)
foreach ($relative in $paths) { $entries[$relative] = Join-Path $projectRoot $relative }
foreach ($entry in (Get-ProjectLicenseFiles).GetEnumerator()) {
    $entries[$entry.Key] = Join-Path $projectRoot $entry.Value
}
foreach ($path in $entries.Values) { Assert-SourceFile $path }
$entries['release-version.json'] = Join-Path $info.BuildRoot 'release-version.json'

# 清单及文件校验完成后才写临时包；任何收集或归档失败均保留已有发布包。
$zipPath = $info.SourceZip
$hashPath = "$zipPath.sha256"
$transactionId = [guid]::NewGuid().ToString('N')
$temporaryZip = "$zipPath.$transactionId.tmp"
$temporaryHash = "$hashPath.$transactionId.tmp"
$backupZip = "$zipPath.$transactionId.backup"
$backupHash = "$hashPath.$transactionId.backup"
New-Item -ItemType Directory -Force -Path (Split-Path $zipPath -Parent) | Out-Null
$fileStream = $null
$archive = $null
try {
    try {
        $fileStream = [IO.File]::Open($temporaryZip, [IO.FileMode]::CreateNew)
        $archive = [IO.Compression.ZipArchive]::new($fileStream, [IO.Compression.ZipArchiveMode]::Create)
        foreach ($entry in $entries.GetEnumerator()) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $archive, $entry.Value, "MSA_Data_Generator_Source/$($entry.Key)",
                [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally {
        try { if ($archive) { $archive.Dispose() } }
        finally { if ($fileStream) { $fileStream.Dispose() } }
    }
    $hash = (Get-FileHash -LiteralPath $temporaryZip -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText($temporaryHash,
        "$hash  $([IO.Path]::GetFileName($zipPath))" + [Environment]::NewLine, [Text.Encoding]::ASCII)
    $zipPublished = $false
    try {
        if ([IO.File]::Exists($zipPath)) { [IO.File]::Replace($temporaryZip, $zipPath, $backupZip) }
        else { [IO.File]::Move($temporaryZip, $zipPath) }
        $zipPublished = $true
        if ([IO.File]::Exists($hashPath)) { [IO.File]::Replace($temporaryHash, $hashPath, $backupHash) }
        else { [IO.File]::Move($temporaryHash, $hashPath) }
    } catch {
        # 校验文件发布失败时回退 ZIP，避免旧校验值对应新归档。
        if ($zipPublished) {
            if ([IO.File]::Exists($backupZip)) {
                [IO.File]::Replace($backupZip, $zipPath, [NullString]::Value)
            }
            else { [IO.File]::Delete($zipPath) }
        }
        throw
    }
    [IO.File]::Delete($backupZip)
    [IO.File]::Delete($backupHash)
} finally {
    [IO.File]::Delete($temporaryZip)
    [IO.File]::Delete($temporaryHash)
}
Get-Item -LiteralPath $zipPath | Select-Object FullName, Length
