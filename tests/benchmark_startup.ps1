param(
    [Parameter(Mandatory)][string]$BaselineExecutable,
    [Parameter(Mandatory)][string]$OptimizedExecutable,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../build/font-startup-benchmark'),
    [int]$Samples = 7
)
$ErrorActionPreference = 'Stop'
if($Samples -lt 3 -or $Samples -gt 30){throw '采样次数须为3～30'}
$root = Split-Path $PSScriptRoot -Parent
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
$runs = [System.Collections.Generic.List[object]]::new()
$executables = [ordered]@{
    baseline = (Resolve-Path -LiteralPath $BaselineExecutable).Path
    optimized = (Resolve-Path -LiteralPath $OptimizedExecutable).Path
}
foreach($variant in $executables.Keys){
    $directory = Join-Path $output $variant
    $profile = Join-Path $directory '用户配置'
    $config = Join-Path $profile 'MSA数据生成器'
    New-Item -ItemType Directory -Path $directory,$config,(Join-Path $directory '空工作目录') -Force | Out-Null
    Copy-Item -LiteralPath $executables[$variant] -Destination (Join-Path $directory 'msa_generator.exe') -Force
    Copy-Item -LiteralPath (Join-Path $root 'examples/示例配置.json') -Destination (Join-Path $config 'msa_generator.json') -Force
}
function Measure-Startup([string]$Variant, [bool]$Warmup){
    $directory = Join-Path $output $Variant
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = Join-Path $directory 'msa_generator.exe'
    $info.WorkingDirectory = Join-Path $directory '空工作目录'
    $info.UseShellExecute = $false
    $info.EnvironmentVariables['USERPROFILE'] = Join-Path $directory '用户配置'
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $process = [Diagnostics.Process]::Start($info)
    try {
        # 框架在首帧绘制完成后才显示窗口，以主窗口首次可见作为外部启动判据。
        do {
            if($process.HasExited){throw '测试程序在显示主窗口前退出'}
            $process.Refresh()
            if($process.MainWindowHandle -ne [IntPtr]::Zero){break}
            if($watch.Elapsed.TotalSeconds -gt 20){throw '测试程序启动超时'}
            Start-Sleep -Milliseconds 5
        } while($true)
        $visibleMs = $watch.Elapsed.TotalMilliseconds
        Start-Sleep -Milliseconds 150
        $process.Refresh()
        if(-not $Warmup){
            $runs.Add([pscustomobject]@{
                variant=$Variant;visible_window_ms=$visibleMs
                cpu_ms=$process.TotalProcessorTime.TotalMilliseconds
                working_set_bytes=$process.WorkingSet64;private_bytes=$process.PrivateMemorySize64
            })
        }
    } finally {
        # 仅终止本函数创建的独立基准进程，没有用户任务或未保存的研究数据。
        if(-not $process.HasExited){$process.Kill();$process.WaitForExit()}
        $process.Dispose()
    }
}
Measure-Startup 'baseline' $true
Measure-Startup 'optimized' $true
for($sample=0;$sample -lt $Samples;$sample++){
    $order = if($sample % 2 -eq 0){@('baseline','optimized')}else{@('optimized','baseline')}
    foreach($variant in $order){Measure-Startup $variant $false}
}
function Median($Values){
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count/2)
    if($sorted.Count % 2){return $sorted[$middle]}
    return ($sorted[$middle-1]+$sorted[$middle])/2
}
$summaries = foreach($variant in $executables.Keys){
    $selected = @($runs | Where-Object {$_.variant -eq $variant})
    [ordered]@{variant=$variant;samples=$selected.Count
        visible_window_ms_median=(Median $selected.visible_window_ms)
        cpu_ms_median=(Median $selected.cpu_ms)
        working_set_bytes_median=(Median $selected.working_set_bytes)
        private_bytes_median=(Median $selected.private_bytes)
        executable_sha256=(Get-FileHash -LiteralPath $executables[$variant]).Hash}
}
[ordered]@{passed=$true;warmup_per_variant=1;samples_per_variant=$Samples
    criterion='主窗口首次可见，框架在首帧绘制后显示窗口；5ms轮询'
    scope='交替顺序的本机预热启动，不能等同所有设备的冷启动'
    summary=@($summaries);runs=$runs} | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath (Join-Path $output 'startup-benchmark.json') -Encoding UTF8
$summaries | ConvertTo-Json
