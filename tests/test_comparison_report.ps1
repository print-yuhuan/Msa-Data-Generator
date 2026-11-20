param([Parameter(Mandatory)][string]$Cli, [Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$run = Join-Path $OutputDirectory ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run -Force | Out-Null
function Invoke-Study([string[]]$Arguments) {
    $lines = & $Cli --no-config @Arguments
    if ($LASTEXITCODE -ne 0) { throw "CLI failed ($LASTEXITCODE): $Arguments" }
    ($lines -join ' ') | ConvertFrom-Json
}
function Read-Report([string]$Path) {
    [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8)
}
function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

$csv = Join-Path $run '研究&A.csv'
$created = Join-Path $run 'generated.html'
$imported = Join-Path $run 'imported.html'
$parameters = @('--lsl','9','--usl','11','--historical-sd','1','--interaction','include',
                '--operator-a','赵<&明','--operator-c','王丽')
$null = Invoke-Study (@('--seed','123','--output',$csv,'--comparison-report',$created) + $parameters)
$xbar = Invoke-Study (@('--input',$csv,'--method','crossed_xbarr','--comparison-report',$imported) + $parameters)
$anova = Invoke-Study (@('--input',$csv,'--method','crossed_anova') + $parameters)
$fromFile = Read-Report $imported
$fromGenerator = Read-Report $created
foreach ($content in @($fromFile, $fromGenerator)) {
    foreach ($key in 'GRR_pct','EV_pct','AV_pct','PT_pct','ndc','GRR','PV','TV') {
        $pattern = '<tr data-metric="' + $key + '"><th[^>]*>[^<]+</th><td data-value="([^"]+)">[^<]+</td><td data-value="([^"]+)">'
        $values = [regex]::Match($content, $pattern)
        Check $values.Success "缺少方法比较指标 $key"
        Check ((ConvertFrom-Json $values.Groups[1].Value) -eq $xbar.metrics.$key) "Xbar-R 的 $key 与独立分析不同"
        Check ((ConvertFrom-Json $values.Groups[2].Value) -eq $anova.metrics.$key) "ANOVA 的 $key 与独立分析不同"
    }
    Check ($content.Contains('强制保留') -and $content.Contains('双侧规格宽度') -and $content.Contains('历史标准差')) '方法或容差来源缺失'
    Check (([regex]::Matches($content, '<svg ').Count -eq 2) -and $content.Contains('@media print')) '报告缺少控制图或打印布局'
    Check ($content.Contains('操作员为赵&lt;&amp;明、B、王丽') -and
           ([regex]::Matches($content, '>操作员 赵&lt;&amp;明</text>').Count -eq 2) -and
           ([regex]::Matches($content, '>操作员 B</text>').Count -eq 2) -and
           ([regex]::Matches($content, '>操作员 王丽</text>').Count -eq 2)) '报告名单和两张控制图应显示已转义的姓名及回退名称'
}
Check ($fromGenerator.Contains('模拟生成') -and $fromGenerator.Contains('数据生成种子 123')) '模拟研究来源缺失'
Check ($fromFile.Contains('数据导入') -and $fromFile.Contains('研究&amp;A.csv') -and -not $fromFile.Contains('数据生成种子')) '导入文件名转义或种子不正确'

$browser = @(
    foreach ($binary in 'msedge.exe','chrome.exe') {
        foreach ($hive in 'HKCU:','HKLM:') {
            $key = Get-Item "$hive\Software\Microsoft\Windows\CurrentVersion\App Paths\$binary" -ErrorAction SilentlyContinue
            if ($key) { $key.GetValue('') }
        }
    }
    foreach ($name in 'ProgramFiles(x86)','ProgramFiles','LOCALAPPDATA') {
        $folder = [Environment]::GetEnvironmentVariable($name)
        if ($folder) {
            Join-Path $folder 'Microsoft\Edge\Application\msedge.exe'
            Join-Path $folder 'Google\Chrome\Application\chrome.exe'
        }
    }
) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
if ($browser) {
    $pdf = Join-Path $run '方法对比 & 结果.PDF'
    $tempDirectory = New-Item -ItemType Directory -Path (Join-Path $run '临时文件 #%')
    $savedTemp, $savedTmp = $env:TEMP, $env:TMP
    try {
        $env:TEMP = $env:TMP = $tempDirectory.FullName
        $null = Invoke-Study (@('--input',$csv,'--comparison-report',$pdf) + $parameters)
        $contents = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($pdf))
        Check ($contents.StartsWith('%PDF-') -and $contents.TrimEnd().EndsWith('%%EOF')) '输出须为完整 PDF'
        Check ([regex]::Matches($contents,'/Type\s*/Page\b').Count -eq 2) '示例 PDF 应包含统计与控制图共两页'
        Check (@(Get-ChildItem -LiteralPath $tempDirectory.FullName -Filter 'msa-print-*' -Force).Count -eq 0) '打印结束后应清理研究和临时配置'
    } finally {
        $env:TEMP, $env:TMP = $savedTemp, $savedTmp
    }
} else {
    Write-Output 'Skipped PDF rendering check: install Microsoft Edge or Google Chrome to enable it.'
}

$constant = Join-Path $run 'constant.csv'
$zeroReport = Join-Path $run 'constant.html'
$longName = '精密测量实验室质量工程组第一工位白班轮值测量操作员'
[IO.File]::WriteAllLines($constant, [string[]](@('1,1,1,1,1') * 4))
$null = Invoke-Study @('--input',$constant,'--operators','2','--operator-a',$longName,'--operator-c','备用姓名','--replicates','2','--parts','5','--comparison-report',$zeroReport)
$zero = Read-Report $zeroReport
Check ($zero.Contains('data-value="null">—') -and $zero.Contains('未定义')) '退化研究应清楚表示缺失的指标或容差'
Check ($zero.Contains("操作员为${longName}、B。") -and -not $zero.Contains('备用姓名')) '两人研究应保留完整长名且只显示前两位操作员'
Write-Output 'Generated, imported and constant comparison studies passed'
