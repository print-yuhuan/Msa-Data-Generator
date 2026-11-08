param(
    [Parameter(Mandatory)][string]$Cli,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'minitab_pt_policy.ps1')
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($outputRoot) | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
$source = Join-Path $outputRoot 'measurements.csv'
[IO.File]::WriteAllText($source, "92,96,100,104,108`r`n94,98,102,106,110`r`n92,94,96,98,100`r`n94,96,98,100,102`r`n", $utf8)
$checks = [Collections.Generic.List[string]]::new()
function Check($Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $checks.Add($Message)
}
function Reject($Action, [string]$Message) {
    $failed = $false
    try { & $Action | Out-Null } catch { $failed = $true }
    Check $failed $Message
}
foreach ($case in @(
    @{name='bilateral';lsl='0';usl='200';tolerance='null';status='compared'},
    @{name='lower';lsl='0';usl='null';tolerance='null';status='not_compared'},
    @{name='upper';lsl='null';usl='200';tolerance='null';status='not_compared'},
    @{name='no-limits';lsl='null';usl='null';tolerance='null';status='not_applicable'},
    @{name='independent';lsl='null';usl='null';tolerance='50';status='not_compared'}
)) {
    $report = Join-Path $outputRoot ($case.name + '.json')
    & $Cli --no-config --input $source --operators 2 --replicates 2 --parts 5 --lsl $case.lsl --usl $case.usl --analysis-tolerance $case.tolerance --report $report --overwrite | Out-Null
    Check ($LASTEXITCODE -eq 0) ($case.name + ' CLI分析成功')
    $native = Get-Content -LiteralPath $report -Raw -Encoding utf8 | ConvertFrom-Json
    # 仅模拟比较器的外部值；没有启动Minitab，不将本脚本记为统计软件实测。
    $external = if ($case.status -eq 'compared') { $native.metrics.PT_pct } else { $null }
    $policy = Test-MinitabPtComparison -Config $native.config -NativeMetrics $native.metrics -MinitabTolerance $external
    Check ($policy.status -eq $case.status -and $policy.compared -eq ($case.status -eq 'compared')) ($case.name + ' 比较范围正确')
    if ($case.name -in 'lower','upper','independent') {
        Check ($null -ne $native.metrics.PT_pct -and [double]$native.metrics.PT_pct -gt 0) ($case.name + ' 可计算P/T未误判为空')
        Check (-not $policy.compared -and -not [string]::IsNullOrWhiteSpace($policy.reason)) ($case.name + ' 明确记录未对标原因')
    }
    if ($case.name -eq 'bilateral') {
        Reject { Test-MinitabPtComparison -Config $native.config -NativeMetrics $native.metrics -MinitabTolerance ([double]$external+1) } '双侧数值不一致必须失败'
        Reject { Test-MinitabPtComparison -Config $native.config -NativeMetrics $native.metrics } '双侧外部值缺失必须失败'
    }
    if ($case.name -eq 'no-limits') {
        $native.metrics.PT_pct = 0
        Reject { Test-MinitabPtComparison -Config $native.config -NativeMetrics $native.metrics } '无规格不接受伪造零P/T'
    }
}
$result = [ordered]@{passed=$true;external_software_executed=$false;checks=$checks.Count;details=$checks.ToArray()}
[IO.File]::WriteAllText((Join-Path $outputRoot 'minitab-policy-regression.json'), ($result | ConvertTo-Json -Depth 5), $utf8)
Write-Output ('Minitab比较判据本地回归通过：' + $checks.Count + ' 项；未启动外部软件。')
