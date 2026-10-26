param([Parameter(Mandatory)][string]$Binary,
      [Parameter(Mandatory)][string]$OutputDirectory,
      [string]$Python = '')
$ErrorActionPreference = 'Stop'
$runRoot = Join-Path $OutputDirectory ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$readyPath = Join-Path $runRoot 'mock-ready.json'
$statsPath = Join-Path $runRoot 'mock-stats.json'
$server = $null
$client = $null

function Start-Hidden([string]$Executable, [string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Executable
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [Text.Encoding]::UTF8
    $info.StandardErrorEncoding = [Text.Encoding]::UTF8
    $info.Arguments = (($Arguments | ForEach-Object {
        '"' + [regex]::Replace([regex]::Replace($_, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
    }) -join ' ')
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    if (-not $process.Start()) { throw '无法启动本地 AI 测试进程。' }
    [pscustomobject]@{process=$process;stdout=$process.StandardOutput.ReadToEndAsync();stderr=$process.StandardError.ReadToEndAsync()}
}

try {
    if (-not $Python) {
        foreach ($candidate in @('python', 'python3')) {
            $found = Get-Command $candidate -ErrorAction SilentlyContinue
            if ($found) { $Python = $found.Source; break }
        }
    }
    if (-not $Python) { throw '本地 AI 模拟服务需要 Python 3；可通过 -Python 指定解释器。' }
    $server = Start-Hidden $Python @('-u', (Join-Path $PSScriptRoot 'mock_ai_server.py'), '--ready', $readyPath, '--stats', $statsPath)
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while (-not (Test-Path -LiteralPath $readyPath)) {
        if ($server.process.HasExited) { throw '本地 AI 模拟服务启动失败。' }
        if ($watch.Elapsed.TotalSeconds -gt 10) { throw '本地 AI 模拟服务启动超时。' }
        Start-Sleep -Milliseconds 100
    }
    $ready = Get-Content -LiteralPath $readyPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $address = 'http://127.0.0.1:' + [int]$ready.port
    $client = Start-Hidden $Binary @('--transport', $address, $runRoot)
    if (-not $client.process.WaitForExit(90000)) {
        $client.process.Kill()
        throw 'AI 本地传输测试超过 90 秒。'
    }
    $exitCode = $client.process.ExitCode
    $clientOutput = $client.stdout.GetAwaiter().GetResult()
    $clientError = $client.stderr.GetAwaiter().GetResult()
    $reportPath = Join-Path $runRoot 'ai-transport-regression.json'
    if (-not (Test-Path -LiteralPath $reportPath)) { throw 'AI 测试进程未生成结果报告。' }
    $report = Get-Content -LiteralPath $reportPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $stats = Get-Content -LiteralPath $statsPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $mockChecks = [System.Collections.Generic.List[object]]::new()
    $mockChecks.Add([pscustomobject]@{name='所有请求符合 JSON 输出协议且只来自回环地址';passed=($stats.invalid_requests -eq 0 -and $stats.non_json_requests -eq 0 -and $stats.non_loopback -eq 0)})
    $mockChecks.Add([pscustomobject]@{name='没有跟随重定向';passed=($null -eq $stats.counts.'redirect-target')})
    foreach ($route in @('success-generate','success-interpret','status-401','status-402','status-429','status-500','unsupported-json','redirect','truncated','empty-choices','malformed-envelope','blank-content','missing-content','finish-length','fenced-content','wrong-task','oversized','timeout','cancel')) {
        $mockChecks.Add([pscustomobject]@{name="请求 $route 仅发送一次，无隐式重试或协议降级";passed=($stats.counts.$route -eq 1)})
    }
    $mockChecks.Add([pscustomobject]@{name='取消后的显式重试产生两次独立请求';passed=($stats.counts.reuse -eq 2)})
    $serviceRoutes = @(
        'models-root','models-v1','models-full','models-duplicate','models-empty','models-unconfigured',
        'models-missing-data','models-not-array','models-numeric-id','models-blank-id',
        'models-control-id','models-long-id','models-missing-id','models-non-object',
        'models-too-many','models-malformed','models-truncated','models-oversized',
        'models-timeout','models-redirect',
        'connection-success','connection-object','connection-deepseek','connection-empty-choices','connection-non-json',
        'connection-fenced','connection-length','connection-array','connection-role','connection-multi',
        'connection-malformed','connection-unsupported-json',
        'balance-success','balance-unavailable','balance-status-402','balance-non-bool','balance-missing-infos',
        'balance-non-array','balance-bad-amount','balance-missing-currency','balance-malformed'
    )
    foreach ($action in @('models','connection','balance')) {
        foreach ($status in @(401,403,429,500)) { $serviceRoutes += "$action-status-$status" }
        $serviceRoutes += "$action-cancel"
    }
    foreach ($route in $serviceRoutes) {
        $method = if ($route.StartsWith('connection-')) { 'POST' } else { 'GET' }
        $mockChecks.Add([pscustomobject]@{name="服务 $route 仅请求一次且使用 $method";passed=($stats.counts.$route -eq 1 -and $stats.service_methods.$route -eq $method)})
    }
    $mockChecks.Add([pscustomobject]@{name='服务成功后取消与显式重试仅发送两次';passed=($stats.counts.'models-reuse' -eq 2)})
    $mockChecks.Add([pscustomobject]@{name='服务查询不跟随重定向';passed=($null -eq $stats.counts.'service-redirect-target')})
    $mockChecks.Add([pscustomobject]@{name='服务前置校验失败没有发送请求';passed=($null -eq $stats.counts.'service-never-send')})
    $mockChecks.Add([pscustomobject]@{name='GET 查询不携带正文且连接测试不发送业务上下文或提示词';passed=($stats.unexpected_get_bodies -eq 0 -and $stats.service_prompt_leaks -eq 0)})
    foreach ($entry in @(
        @('models-root','/models'), @('models-v1','/v1/models'), @('models-full','/models-full/models'),
        @('connection-success','/connection-success/chat/completions'),
        @('balance-success','/balance-success/user/balance')
    )) {
        $route, $expected = $entry
        $mockChecks.Add([pscustomobject]@{name="服务路径规范化：$route";passed=($stats.service_paths.$route -eq $expected)})
    }
    $failedMock = @($mockChecks | Where-Object { -not $_.passed }).Count
    $passed = $exitCode -eq 0 -and $report.passed -and $failedMock -eq 0
    $summary = [ordered]@{passed=$passed;real_network=$false;checks=@($report.checks) + $mockChecks.ToArray();count=([int]$report.count + $mockChecks.Count);failed=([int]$report.failed + $failedMock);mock_request_counts=$stats.counts;case_report=$reportPath}
    $summary | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'ai-transport-regression.json') -Encoding UTF8
    Write-Output $clientOutput.Trim()
    if (-not $passed) {
        if ($clientError.Trim()) { Write-Output $clientError.Trim() }
        foreach ($item in $mockChecks) { if (-not $item.passed) { Write-Output $item.name } }
        throw 'AI 本地传输回归检查存在失败项，请查看安全测试报告。'
    }
    Write-Output ("AI 本地 HTTP 模拟检查通过：{0} 项；未访问真实服务。" -f $summary.count)
} finally {
    foreach ($job in @($client, $server)) {
        if ($job) {
            if (-not $job.process.HasExited) { $job.process.Kill(); $job.process.WaitForExit(5000) | Out-Null }
            $job.process.Dispose()
        }
    }
}
