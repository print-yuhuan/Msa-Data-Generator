param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$projectRoot = Split-Path $PSScriptRoot -Parent
$runRoot = [IO.Path]::GetFullPath((Join-Path $OutputDirectory ([guid]::NewGuid().ToString('N'))))
$fixtureRoot = Join-Path $runRoot '中文 源码仓库'
New-Item -ItemType Directory -Force -Path $fixtureRoot | Out-Null
$checks = [Collections.Generic.List[object]]::new()
function Check([bool]$Passed, [string]$Name) {
    if (-not $Passed) { throw "FAIL: $Name" }
    $checks.Add([pscustomobject]@{name = $Name; passed = $true})
}
function Write-Fixture([string]$Relative, [string]$Text) {
    $path = Join-Path $fixtureRoot $Relative
    New-Item -ItemType Directory -Force -Path (Split-Path $path -Parent) | Out-Null
    [IO.File]::WriteAllText($path, $Text, [Text.UTF8Encoding]::new($false))
}
function Run-Hidden([string]$Executable, [string[]]$Arguments) {
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
    $startInfo.StandardErrorEncoding = [Text.Encoding]::UTF8
    $startInfo.Arguments = (($Arguments | ForEach-Object {
        '"' + [regex]::Replace([regex]::Replace($_, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
    }) -join ' ')
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) { throw '无法启动隔离测试进程' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            throw '源码打包回归超时'
        }
        [pscustomobject]@{exit = $process.ExitCode; stdout = $stdout.GetAwaiter().GetResult();
                         stderr = $stderr.GetAwaiter().GetResult()}
    } finally { $process.Dispose() }
}
function Run-Git([string[]]$Arguments) {
    $result = Run-Hidden 'git' (@('-C', $fixtureRoot) + $Arguments)
    if ($result.exit -ne 0) { throw "隔离仓库命令失败：$($result.stderr)" }
}
$shellPath = (Get-Process -Id $PID).Path
function Run-Package {
    $scriptPath = (Join-Path $fixtureRoot 'scripts/package-source.ps1').Replace("'", "''")
    $command = '[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false); & ''' +
        $scriptPath + ''' -BuildDirectory build; if (-not $?) { exit 1 }'
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    Run-Hidden $shellPath @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', $encoded)
}
function Read-Archive {
    $contents = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::Ordinal)
    $archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        foreach ($entry in $archive.Entries) {
            Check (-not $contents.ContainsKey($entry.FullName)) "归档路径唯一：$($entry.FullName)"
            $reader = [IO.StreamReader]::new($entry.Open(), [Text.Encoding]::UTF8)
            try { $contents.Add($entry.FullName, $reader.ReadToEnd()) }
            finally { $reader.Dispose() }
        }
    } finally { $archive.Dispose() }
    ,$contents
}

# 只创建本地临时仓库，无提交、联网、真实密钥或个人配置读取。
Run-Git @('init', '--quiet')
Run-Git @('config', 'core.autocrlf', 'false')
Run-Git @('config', 'core.quotePath', 'true')
$ignore = @'
/build*/
/dist/
**/__pycache__/
*.tmp
*.log
*.exe
.env
.env.*
ai_settings.json
!.env.example
/docs/diagnostics/
'@
foreach ($relative in @('CMakeLists.txt', 'LICENSE', 'NOTICE', 'README.md',
    'THIRD_PARTY_NOTICES.md', 'dependencies.lock.json', '.clang-format', '.gitattributes')) {
    Write-Fixture $relative "fixture: $relative"
}
Write-Fixture '.gitignore' $ignore
Write-Fixture 'src/已跟踪 源文件.cpp' 'staged original'
Write-Fixture 'src/已删除.cpp' 'deleted after staging'
Write-Fixture 'private-outside-allowlist.txt' 'tracked but excluded by source directory policy'
Write-Fixture 'licenses/MSA-Project-LICENSE.txt' 'stale duplicate to be replaced'
Write-Fixture 'licenses/MSA-Project-NOTICE.txt' 'stale duplicate to be replaced'
foreach ($script in 'package-source.ps1', 'release-common.ps1') {
    New-Item -ItemType Directory -Force -Path (Join-Path $fixtureRoot 'scripts') | Out-Null
    Copy-Item -LiteralPath (Join-Path $projectRoot "scripts/$script") -Destination (Join-Path $fixtureRoot 'scripts')
}
Run-Git @('add', '--all')
Write-Fixture 'src/已跟踪 源文件.cpp' 'current worktree content: 未提交修改'
[IO.File]::Delete((Join-Path $fixtureRoot 'src/已删除.cpp'))
Write-Fixture 'src/新增 源文件.cpp' 'new untracked source'
Write-Fixture 'src/嵌套/.gitignore' 'local-private.txt'
Write-Fixture 'src/嵌套/local-private.txt' 'FAKE_PRIVATE_MARKER'
Write-Fixture 'tests/.env' 'FAKE_PRIVATE_MARKER'
Write-Fixture 'tests/ai_settings.json' '{"fake_review_credential":"FAKE_PRIVATE_MARKER"}'
Write-Fixture 'tests/temporary.tmp' 'FAKE_PRIVATE_MARKER'
Write-Fixture 'tests/__pycache__/fixture.pyc' 'FAKE_PRIVATE_MARKER'
Write-Fixture 'tests/.env.example' 'safe example'
Write-Fixture 'tests/info-excluded.txt' 'FAKE_PRIVATE_MARKER'
Write-Fixture '.git/info/exclude' 'tests/info-excluded.txt'
Write-Fixture 'docs/diagnostics/private.md' 'FAKE_PRIVATE_MARKER'
Write-Fixture 'docs/使用说明.md' 'public documentation'
Write-Fixture 'docs/图示.png' 'fixture image bytes'
Write-Fixture 'docs/not-in-document-types.pdf' 'excluded documentation type'
Write-Fixture 'build/release-version.json' '{"version":"1.0.0","native_version":"1.0.0-native"}'
Write-Fixture 'build/temporary.exe' 'FAKE_PRIVATE_MARKER'
Write-Fixture '.github/workflows/build.yml' 'fixture workflow'
Write-Fixture 'third_party/fixture/header.h' 'bundled dependency source'
Write-Fixture 'resources/fonts/fixture.otf' 'bundled resource bytes'
$zipPath = Join-Path $fixtureRoot 'dist/MSA_Data_Generator_v1.0.0_Source.zip'
$hashPath = "$zipPath.sha256"
$package = Run-Package
Check ($package.exit -eq 0) "中文目录源码打包成功：$($package.stderr)"
$contents = Read-Archive
$prefix = 'MSA_Data_Generator_Source/'
Check ($contents[$prefix + 'src/已跟踪 源文件.cpp'] -ceq 'current worktree content: 未提交修改') '已跟踪文件使用当前工作区内容'
Check ($contents[$prefix + 'src/新增 源文件.cpp'] -ceq 'new untracked source') '未忽略的新源码纳入归档'
Check (-not $contents.ContainsKey($prefix + 'src/已删除.cpp')) '已删除但尚未提交的跟踪文件不进入归档'
Check ($contents[$prefix + 'tests/.env.example'] -ceq 'safe example') '忽略规则的显式例外仍被保留'
foreach ($excluded in @('tests/.env', 'tests/ai_settings.json', 'tests/temporary.tmp',
    'tests/__pycache__/fixture.pyc', 'src/嵌套/local-private.txt', 'tests/info-excluded.txt',
    'docs/diagnostics/private.md', 'docs/not-in-document-types.pdf',
    'build/temporary.exe', 'private-outside-allowlist.txt')) {
    Check (-not $contents.ContainsKey($prefix + $excluded)) "排除受忽略规则或目录白名单限制的文件：$excluded"
}
Check (@($contents.Values | Where-Object { $_.Contains('FAKE_PRIVATE_MARKER') }).Count -eq 0) '归档不包含任何伪造私密标记'
foreach ($required in @('docs/使用说明.md', 'docs/图示.png', '.github/workflows/build.yml',
    'third_party/fixture/header.h', 'resources/fonts/fixture.otf', 'release-version.json')) {
    Check ($contents.ContainsKey($prefix + $required)) "保留源码构建与文档材料：$required"
}
Check ($contents[$prefix + 'licenses/MSA-Project-LICENSE.txt'] -ceq $contents[$prefix + 'LICENSE']) '专用许可副本与根许可证一致且无重复项'
Check ($contents[$prefix + 'licenses/MSA-Project-NOTICE.txt'] -ceq $contents[$prefix + 'NOTICE']) '专用署名副本与根 NOTICE 一致且无重复项'
$expectedHash = (Get-FileHash -LiteralPath $zipPath).Hash.ToLowerInvariant()
Check ([IO.File]::ReadAllText($hashPath).StartsWith($expectedHash + '  ')) '校验文件对应实际源码包'

# 使用已有包验证清单失败、归档读失败及校验文件写失败均不破坏旧包。
$zipBefore = (Get-FileHash -LiteralPath $zipPath).Hash
$hashBefore = (Get-FileHash -LiteralPath $hashPath).Hash
$licensePath = Join-Path $fixtureRoot 'LICENSE'
$licenseText = [IO.File]::ReadAllText($licensePath)
[IO.File]::Delete($licensePath)
try {
    $failure = Run-Package
    Check ($failure.exit -ne 0) '必需文件缺失时拒绝打包'
    Check ((Get-FileHash -LiteralPath $zipPath).Hash -eq $zipBefore -and
           (Get-FileHash -LiteralPath $hashPath).Hash -eq $hashBefore) '清单收集失败保留已有包及校验文件'
} finally { Write-Fixture 'LICENSE' $licenseText }
$locked = [IO.File]::Open((Join-Path $fixtureRoot 'src/已跟踪 源文件.cpp'),
    [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
try {
    $failure = Run-Package
    Check ($failure.exit -ne 0) '源文件无法读取时拒绝打包'
    Check ((Get-FileHash -LiteralPath $zipPath).Hash -eq $zipBefore -and
           (Get-FileHash -LiteralPath $hashPath).Hash -eq $hashBefore) '归档写入失败保留已有包及校验文件'
} finally { $locked.Dispose() }
$locked = [IO.File]::Open($hashPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    Write-Fixture 'src/新增 源文件.cpp' 'changed content before failed publication'
    $failure = Run-Package
    Check ($failure.exit -ne 0) '校验文件被占用时拒绝发布新包'
    Check ((Get-FileHash -LiteralPath $zipPath).Hash -eq $zipBefore -and
           (Get-FileHash -LiteralPath $hashPath).Hash -eq $hashBefore) '校验文件发布失败回退 ZIP 并保留原校验值'
} finally { $locked.Dispose() }
$replacement = Run-Package
Check ($replacement.exit -eq 0) '解锁后可重新生成并替换源码包'
$expectedHash = (Get-FileHash -LiteralPath $zipPath).Hash.ToLowerInvariant()
Check ([IO.File]::ReadAllText($hashPath).StartsWith($expectedHash + '  ')) '重新发布后校验值匹配'
$outside = Join-Path $runRoot 'outside-source'
New-Item -ItemType Directory -Path $outside | Out-Null
[IO.File]::WriteAllText((Join-Path $outside 'private.txt'), 'FAKE_PRIVATE_MARKER')
$junction = Join-Path $fixtureRoot 'src/external-link'
New-Item -ItemType Junction -Path $junction -Target $outside | Out-Null
try {
    $zipBefore = (Get-FileHash -LiteralPath $zipPath).Hash
    $failure = Run-Package
    Check ($failure.exit -ne 0) '拒绝经目录联接收集工作区外文件'
    Check ((Get-FileHash -LiteralPath $zipPath).Hash -eq $zipBefore) '目录联接校验失败保留已有包'
} finally {
    # 只删除本测试创建的联接本身，不递归删除其目标目录。
    [IO.Directory]::Delete($junction)
}
Check (@(Get-ChildItem -LiteralPath (Split-Path $zipPath -Parent) -File |
    Where-Object { $_.Name.EndsWith('.tmp') -or $_.Name.EndsWith('.backup') }).Count -eq 0) '成功或受控失败后没有残留临时归档'
[ordered]@{passed = $true; checks = $checks; run_directory = $runRoot; real_network = $false} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'source-package-regression.json') -Encoding UTF8
Write-Output "$($checks.Count) source package regression checks PASS"
