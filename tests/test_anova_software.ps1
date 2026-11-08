param(
    [string]$Matlab='C:/Program Files/MATLAB/R2026b/bin/matlab.exe',
    [string]$SpssPython='C:/Program Files/IBM/SPSS Statistics/Python3/python.exe',
    [string]$OutputDirectory=(Join-Path $PSScriptRoot ('../build/anova-software-'+(Get-Date -Format 'yyyyMMdd-HHmmss')))
)
# 可选集成测试：需要本机已授权的五款软件，不属于常规CTest依赖。
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$output=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $output){throw '请使用新的输出目录，保留已有软件实测证据'}
$work=Join-Path ([IO.Path]::GetTempPath()) ('msa-anova-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output | Out-Null
& python (Join-Path $PSScriptRoot 'anova/prepare.py') $work
if($LASTEXITCODE-ne0){throw '共用CSV准备失败'}
$manifest=Join-Path $work 'manifest.json'
& (Join-Path $PSScriptRoot 'anova/minitab_compare.ps1') -Manifest $manifest
& (Join-Path $PSScriptRoot 'anova/jmp_compare.ps1') -Manifest $manifest
$previousPath=$env:PATH
try{
    $env:PATH=(Split-Path $SpssPython -Parent)+';'+(Split-Path (Split-Path $SpssPython -Parent) -Parent)+';'+$previousPath
    & $SpssPython (Join-Path $PSScriptRoot 'anova/spss_compare.py') $manifest > (Join-Path $work 'spss.log') 2>&1
    if($LASTEXITCODE-ne0){throw 'SPSS实际计算失败，详见日志'}
}finally{$env:PATH=$previousPath}
$scripts=(Join-Path $PSScriptRoot 'anova').Replace('\','/').Replace("'","''")
$inputPath=$manifest.Replace('\','/').Replace("'","''")
& $Matlab -batch "addpath('$scripts');matlab_compare('$inputPath');" -logfile (Join-Path $work 'matlab.log')
if($LASTEXITCODE-ne0){throw 'MATLAB实际计算失败'}
& (Join-Path $PSScriptRoot 'anova/origin_compare.ps1') -Manifest $manifest
& python (Join-Path $PSScriptRoot 'anova/verify.py') $work
if($LASTEXITCODE-ne0){throw '五软件数值比较失败'}
# 完整证据保留原测试根目录；复制到项目内供审阅，不移动或删除外部目录。
Get-ChildItem -LiteralPath $work -Force | Copy-Item -Destination $output -Recurse
$copiedManifest=Get-Content -LiteralPath $manifest -Raw -Encoding utf8 | ConvertFrom-Json
foreach($case in $copiedManifest.cases){
    $case.folder=Join-Path $output $case.id
    $case.csv=Join-Path $case.folder 'data.csv';$case.config=Join-Path $case.folder 'config.json';$case.native=Join-Path $case.folder 'native.json'
}
$copiedManifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $output 'manifest.json') -Encoding utf8
$binding=[ordered]@{
    passed=$true;created_utc=[DateTime]::UtcNow.ToString('o');source_directory=$work
    cli_sha256=(Get-FileHash -LiteralPath (Join-Path $root 'build/Release/msa_cli.exe')).Hash
    gui_sha256=(Get-FileHash -LiteralPath (Join-Path $root 'build/Release/msa_generator.exe')).Hash
    fixture_sha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'fixtures/anova_software_baseline.json')).Hash
}
$binding | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'artifact-binding.json') -Encoding utf8
Write-Output ('五软件实际对标通过，证据：'+$output)
