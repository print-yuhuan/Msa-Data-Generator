param([Parameter(Mandatory)][string]$Cli,
      [Parameter(Mandatory)][string]$OutputDirectory,
      [Parameter(Mandatory)][string]$ExpectedVersion)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$runRoot = Join-Path $OutputDirectory ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$checks = [System.Collections.Generic.List[object]]::new()
function Check([bool]$ok, [string]$name) {
    if (-not $ok) { throw "FAIL: $name" }
    $checks.Add([pscustomobject]@{name=$name;passed=$true})
}
function Start-Cli([string[]]$Arguments, [bool]$WithoutProfile = $false, [string]$ProfileRoot = '') {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Cli
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [Text.Encoding]::UTF8
    $info.StandardErrorEncoding = [Text.Encoding]::UTF8
    $info.Arguments = (($Arguments | ForEach-Object {
        '"' + [regex]::Replace([regex]::Replace($_, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
    }) -join ' ')
    if ($WithoutProfile) { $info.EnvironmentVariables.Remove('USERPROFILE') }
    if ($ProfileRoot) { $info.EnvironmentVariables['USERPROFILE'] = $ProfileRoot }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    if (-not $process.Start()) { throw 'CLI start failed' }
    [pscustomobject]@{process=$process; stdout=$process.StandardOutput.ReadToEndAsync(); stderr=$process.StandardError.ReadToEndAsync()}
}
function Finish-Cli($Job) {
    try {
        if (-not $Job.process.WaitForExit(30000)) {
            $Job.process.Kill()
            throw 'CLI regression timed out'
        }
        [pscustomobject]@{exit=$Job.process.ExitCode;stdout=$Job.stdout.GetAwaiter().GetResult();stderr=$Job.stderr.GetAwaiter().GetResult()}
    } finally { $Job.process.Dispose() }
}
function Run-Cli([string[]]$Arguments, [bool]$WithoutProfile = $false, [string]$ProfileRoot = '') {
    Finish-Cli (Start-Cli $Arguments $WithoutProfile $ProfileRoot)
}
$config = Join-Path $runRoot '中文 配置.json'
Copy-Item -LiteralPath (Join-Path $root 'examples\示例配置.json') -Destination $config
$before = (Get-FileHash -LiteralPath $config).Hash
$help = Run-Cli @('--help') $true
Check ($help.exit -eq 0 -and $help.stdout.Contains("$ExpectedVersion-native")) 'help without USERPROFILE and generated version'
Check ($help.stdout.Contains('print-yuhuan') -and $help.stdout.Contains('https://github.com/print-yuhuan/Msa-Data-Generator.git') -and $help.stdout.Contains('LICENSE') -and $help.stdout.Contains('NOTICE')) 'CLI help identifies author, repository and license information'
$noArgs = Run-Cli @() $true
Check ($help.stdout.Contains('MIT License') -and $help.stdout.Contains('--license') -and $help.stdout.Contains('licenses/MSA-Project-LICENSE.txt') -and $help.stdout.Contains('允许商业使用') -and $help.stdout.Contains('版权声明与许可声明') -and $help.stdout -notmatch '非商业|禁止盈利|须公开|LicenseRef') 'help describes MIT permissions, notice retention and full license retrieval without additional restrictions'
$license = Run-Cli @('--license') $true
$licenseSource = [IO.File]::ReadAllText((Join-Path $root 'LICENSE'))
Check ($license.exit -eq 0 -and $license.stdout.Replace("`r`n","`n") -ceq $licenseSource.Replace("`r`n","`n")) 'embedded full license matches source without USERPROFILE'
$invalidLicense = Run-Cli @('--license=summary') $true
Check ($invalidLicense.exit -eq 2) 'license flag rejects unexpected values'
Check ($noArgs.exit -eq 2) 'launching CLI without arguments does not export files'
$init = Run-Cli @('--init','--config',$config) $true
Check ($init.exit -eq 0 -and (Get-FileHash -LiteralPath $config).Hash -eq $before) 'explicit init without USERPROFILE preserves existing config'
$newConfig=Join-Path $runRoot 'new-default.json'
$newInit=Run-Cli @('--init','--config',$newConfig) $true
$newDefaults=Get-Content -LiteralPath $newConfig -Raw -Encoding UTF8 | ConvertFrom-Json
Check ($newInit.exit -eq 0 -and $newDefaults.output.format -eq 'default' -and $newDefaults.output.run_order.mode -eq 'standard') 'init creates complete defaults including new output settings'
$csv = Join-Path $runRoot '中文 数据.csv'
$report = Join-Path $runRoot '中文 报告.json'
$explicit = Run-Cli @('--config',$config,'--output',$csv,'--report',$report) $true
Check ($explicit.exit -eq 0) 'explicit generation without USERPROFILE'
$result = Get-Content -LiteralPath $report -Raw -Encoding UTF8 | ConvertFrom-Json
Check ($result.version -eq "$ExpectedVersion-native") 'CLI report version equals CMake version'
$missing = Run-Cli @('--output',(Join-Path $runRoot 'missing-profile.csv')) $true
Check ($missing.exit -eq 1 -and $missing.stderr.Length -gt 0) 'missing default profile returns controlled IO error'
$savedHash = (Get-FileHash -LiteralPath $csv).Hash
$existing = Run-Cli @('--config',$config,'--output',$csv)
Check ($existing.exit -eq 2 -and (Get-FileHash -LiteralPath $csv).Hash -eq $savedHash) 'existing CSV protected without overwrite'
$replace = Run-Cli @('--config',$config,'--output',$csv,'--report',$report,'--overwrite')
Check ($replace.exit -eq 0) 'explicit overwrite remains supported'

$namingData = Get-Content -LiteralPath $config -Raw -Encoding UTF8 | ConvertFrom-Json
$namingData.generation.seed = 123
$namingData.output.directory = Join-Path $runRoot '模板 导出'
$namingData.output.filename = '研究_[OPERATORS]_[SEED].csv'
$namingData.output | Add-Member -NotePropertyName existing_file -NotePropertyValue 'number' -Force
$namingConfig = Join-Path $runRoot 'naming.json'
$namingData | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $namingConfig -Encoding UTF8
$named = Run-Cli @('--config',$namingConfig) $true
$namedAgain = Run-Cli @('--config',$namingConfig) $true
Check ($named.exit -eq 0 -and $namedAgain.exit -eq 0) 'configured template export works without USERPROFILE for absolute output'
$namedPath = ($named.stdout | ConvertFrom-Json).output
$numberedPath = ($namedAgain.stdout | ConvertFrom-Json).output
Check ([IO.Path]::GetFileName($namedPath) -eq '研究_3_123.csv' -and [IO.Path]::GetFileName($numberedPath) -eq '研究_3_123_001.csv') 'configured template uses actual seed and numbers collisions'
$literalPath = Join-Path $runRoot 'literal_[SEED].csv'
$literal = Run-Cli @('--config',$namingConfig,'--output',$literalPath) $true
Check ($literal.exit -eq 0 -and (Test-Path -LiteralPath $literalPath)) 'explicit output brackets remain literal'
$namingData.output.filename = '[UNKNOWN].csv'
$namingData | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $namingConfig -Encoding UTF8
$badTemplate = Run-Cli @('--config',$namingConfig)
Check ($badTemplate.exit -eq 1 -and $badTemplate.stderr.Contains('[UNKNOWN]')) 'unknown filename token is a controlled configuration error'

$namingData.output.filename = 'long.csv'
$namingData.output | Add-Member -NotePropertyName format -NotePropertyValue 'minitab' -Force
$namingData.output | Add-Member -NotePropertyName run_order -NotePropertyValue ([pscustomobject]@{mode='within_operator';seed=321;randomize_operators=$true;store_standard_order=$true}) -Force
$namingData | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $namingConfig -Encoding UTF8
$longPath=Join-Path $runRoot 'explicit-long.csv'
$longReport=Join-Path $runRoot 'long-report.json'
$long=Run-Cli @('--config',$namingConfig,'--output',$longPath,'--report',$longReport) $true
Check ($long.exit -eq 0) 'explicit CLI output honors configured long format and randomization'
$longBytes=[IO.File]::ReadAllBytes($longPath)
Check (-not ($longBytes[0] -eq 239 -and $longBytes[1] -eq 187 -and $longBytes[2] -eq 191)) 'long CLI CSV has no BOM'
$longRows=@(Import-Csv -LiteralPath $longPath -Encoding UTF8)
$longResult=Get-Content -LiteralPath $longReport -Raw -Encoding UTF8 | ConvertFrom-Json
Check ($longRows.Count -eq ($namingData.design.operators*$namingData.design.replicates*$namingData.design.parts)) 'long CLI record count'
$mapping=$true
foreach($row in $longRows){
    $op=[int][char]$row.Operator-[int][char]'A'
    $trial=[int]$row.Trial; $part=[int]$row.Part
    $value=$longResult.matrix[$op*$namingData.design.replicates+$trial-1][$part-1]
    $expectedValue=$value.ToString('F'+$namingData.generation.decimal_places,[Globalization.CultureInfo]::InvariantCulture)
    if($row.Measurement -cne $expectedValue -or [int]$row.StdOrder -ne (($trial-1)*$namingData.design.parts+$part-1)*$namingData.design.operators+$op+1){$mapping=$false}
}
Check $mapping 'long CLI Trial, StdOrder and measurement precision map to the report matrix'

# Every GUI business field is overridden, even when --config appears after the options.
$overrideData=Get-Content -LiteralPath $config -Raw -Encoding UTF8 | ConvertFrom-Json
$overrideData.design.operators=1 # Replaced before shared configuration validation.
$overrideData.targets.EV_pct | Add-Member -NotePropertyName tolerance_pp -NotePropertyValue 8 -Force
$overrideData.targets.PT_pct=[pscustomobject]@{mode='invalid'}
$overrideConfig=Join-Path $runRoot 'override.json'
$overrideData | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $overrideConfig -Encoding UTF8
$overrideHash=(Get-FileHash -LiteralPath $overrideConfig).Hash
$overrideDirectory=Join-Path $runRoot '业务参数 导出'
$overrideReport=Join-Path $runRoot 'override-report.json'
$business=@('--operators=2','--replicates','3','--trials','2','--parts','10','--parts','5',
    '--study-multiplier','5.15','--lsl','null','--usl','null','--compliance','relaxed',
    '--center','12.5','--priority','indicators','--scale-mode','reference_tv_sd',
    '--reference-tv-sd','0.2','--decimal-places','6','--seed','-123','--max-attempts','50',
    '--timeout-seconds','5','--percentage-tolerance-pp','1','--ndc-tolerance','2',
    '--ev','target:10:1','--av','between:0:20','--grr','at_most:30','--pt','none','--ndc','at_least:1',
    '--output-directory',$overrideDirectory,'--filename','命令行_[OPERATORS]_[SEED].csv',
    '--existing-file','number','--csv-format','minitab','--run-order','within_operator',
    '--randomize-operators','true','--run-seed','321','--store-standard-order','true')
$overridden=Run-Cli ($business+@('--config',$overrideConfig,'--report',$overrideReport)) $true
Check ($overridden.exit -eq 0) 'all GUI business overrides generate successfully before or after --config'
$effective=(Get-Content -LiteralPath $overrideReport -Raw -Encoding UTF8 | ConvertFrom-Json).config
Check ($effective.design.operators -eq 2 -and $effective.design.replicates -eq 2 -and $effective.design.parts -eq 5) 'design overrides and repeated aliases use last value'
Check ($effective.analysis.study_multiplier -eq 5.15 -and $null -eq $effective.specification.lsl -and $null -eq $effective.specification.usl -and $effective.specification.compliance -eq 'relaxed') 'analysis and nullable specification overrides'
Check ($effective.generation.center -eq 12.5 -and $effective.generation.priority -eq 'indicators' -and $effective.generation.scale_mode -eq 'reference_tv_sd' -and $effective.generation.reference_tv_sd -eq 0.2 -and $effective.generation.decimal_places -eq 6 -and $effective.generation.seed -eq -123 -and $effective.generation.max_attempts -eq 50 -and $effective.generation.timeout_seconds -eq 5) 'all generation and search overrides'
Check ($effective.target_defaults.percentage_tolerance_pp -eq 1 -and $effective.target_defaults.ndc_tolerance -eq 2) 'default target tolerance overrides'
Check ($effective.targets.EV_pct.mode -eq 'target' -and $effective.targets.EV_pct.value -eq 10 -and $effective.targets.EV_pct.tolerance_pp -eq 1 -and $effective.targets.AV_pct.mode -eq 'between' -and $effective.targets.AV_pct.min -eq 0 -and $effective.targets.AV_pct.max -eq 20 -and $effective.targets.GRR_pct.mode -eq 'at_most' -and $effective.targets.GRR_pct.value -eq 30 -and $null -eq $effective.targets.PT_pct -and $effective.targets.ndc.mode -eq 'at_least' -and $effective.targets.ndc.value -eq 1) 'all five metric overrides replace complete target objects'
Check ($effective.output.directory -eq $overrideDirectory -and $effective.output.filename -eq '命令行_[OPERATORS]_[SEED].csv' -and $effective.output.existing_file -eq 'number' -and $effective.output.format -eq 'minitab' -and $effective.output.run_order.mode -eq 'within_operator' -and $effective.output.run_order.randomize_operators -and $effective.output.run_order.seed -eq 321 -and $effective.output.run_order.store_standard_order) 'all output and run-order overrides'
$overrideCsv=($overridden.stdout | ConvertFrom-Json).output
Check ([IO.Path]::GetFileName($overrideCsv) -eq '命令行_2_-123.csv' -and @(Import-Csv -LiteralPath $overrideCsv).Count -eq 20) 'overridden filename variables and study dimensions reach actual CSV'
$again=Run-Cli ($business+@('--config',$overrideConfig,'--csv-format','jmp19')) $true
$againCsv=($again.stdout | ConvertFrom-Json).output
Check ($again.exit -eq 0 -and [IO.Path]::GetFileName($againCsv) -eq '命令行_2_-123_001.csv' -and (Get-FileHash -LiteralPath $againCsv).Hash -eq (Get-FileHash -LiteralPath $overrideCsv).Hash) 'CLI selected software formats preserve fixed order and configured numbering'
Check ((Get-FileHash -LiteralPath $overrideConfig).Hash -eq $overrideHash) 'business overrides never rewrite source config'

$savedEffective=Join-Path $runRoot 'effective.json'
$effective | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $savedEffective -Encoding UTF8
$clearReport=Join-Path $runRoot 'clear-report.json'
$cleared=Run-Cli @('--config',$savedEffective,'--seed','null','--center','null','--reference-tv-sd','null',
    '--scale-mode','auto','--lsl','9','--usl','11','--compliance','strict','--priority','center',
    '--ev','none','--av','none','--grr','none','--pt','at_least:0','--ndc','none',
    '--run-seed','null','--randomize-operators','false','--store-standard-order','false',
    '--csv-format','default','--run-order','standard','--existing-file','overwrite',
    '--output',(Join-Path $runRoot 'cleared.csv'),'--report',$clearReport) $true
Check ($cleared.exit -eq 0) 'nullable and boolean overrides can clear saved GUI settings'
$clearedConfig=(Get-Content -LiteralPath $clearReport -Raw -Encoding UTF8 | ConvertFrom-Json).config
Check ($null -eq $clearedConfig.generation.seed -and $null -eq $clearedConfig.generation.center -and $null -eq $clearedConfig.generation.reference_tv_sd -and $null -eq $clearedConfig.output.run_order.seed -and -not $clearedConfig.output.run_order.randomize_operators -and -not $clearedConfig.output.run_order.store_standard_order) 'explicit null and false survive defaults and saved configuration'

$omittedReport=Join-Path $runRoot 'omitted-tolerance.json'
$omitted=Run-Cli @('--config',$savedEffective,'--ev','target:10','--ndc','target:9:0',
    '--csv-format','spss','--run-order','all','--output',(Join-Path $runRoot 'omitted-tolerance.csv'),'--report',$omittedReport) $true
Check ($omitted.exit -eq 0) 'target shorthand supports omitted tolerance and zero integer tolerance'
$omittedCfg=(Get-Content -LiteralPath $omittedReport -Raw -Encoding UTF8 | ConvertFrom-Json).config
Check (-not $omittedCfg.targets.EV_pct.PSObject.Properties['tolerance_pp'] -and $omittedCfg.targets.ndc.tolerance -eq 0 -and $omittedCfg.output.run_order.mode -eq 'all') 'replacing target removes stale explicit tolerance and preserves zero'

$standaloneReport=Join-Path $runRoot 'standalone-report.json'
$standalone=Run-Cli @('--no-config','--lsl','9','--usl','11','--seed','123',
    '--csv-format','origin','--output',(Join-Path $runRoot 'standalone.csv'),'--report',$standaloneReport) $true
Check ($standalone.exit -eq 0) 'no-config business options work without USERPROFILE or JSON input'
$standaloneCfg=(Get-Content -LiteralPath $standaloneReport -Raw -Encoding UTF8 | ConvertFrom-Json).config
Check ($standaloneCfg.design.parts -eq 10 -and $standaloneCfg.output.format -eq 'origin') 'unspecified standalone fields retain built-in defaults'

$fakeProfile=Join-Path $runRoot 'fake-profile'
$fakeConfig=Join-Path $fakeProfile 'MSA数据生成器/msa_generator.json'
New-Item -ItemType Directory -Force -Path (Split-Path $fakeConfig -Parent) | Out-Null
Copy-Item -LiteralPath $savedEffective -Destination $fakeConfig
$fakeHash=(Get-FileHash -LiteralPath $fakeConfig).Hash
$implicitReport=Join-Path $runRoot 'implicit-report.json'
$implicit=Run-Cli @('--parts','6','--csv-format','matlab','--output',(Join-Path $runRoot 'implicit.csv'),'--report',$implicitReport) $false $fakeProfile
Check ($implicit.exit -eq 0) 'business options overlay implicit GUI user configuration'
$implicitCfg=(Get-Content -LiteralPath $implicitReport -Raw -Encoding UTF8 | ConvertFrom-Json).config
Check ($implicitCfg.design.parts -eq 6 -and $implicitCfg.generation.seed -eq -123 -and $implicitCfg.output.format -eq 'matlab' -and (Get-FileHash -LiteralPath $fakeConfig).Hash -eq $fakeHash) 'implicit config provides remaining fields and remains unchanged'
$emptyProfile=Join-Path $runRoot 'empty-profile'
$memory=Run-Cli @('--lsl','9','--usl','11','--seed','123','--output',(Join-Path $runRoot 'memory.csv')) $false $emptyProfile
Check ($memory.exit -eq 0 -and -not (Test-Path -LiteralPath $emptyProfile)) 'missing implicit config uses memory defaults without creating user files'
$missingConfig=Join-Path $runRoot 'missing-input.json'
$missingExplicit=Run-Cli @('--config',$missingConfig,'--output',(Join-Path $runRoot 'missing-input.csv')) $true
Check ($missingExplicit.exit -eq 1 -and -not (Test-Path -LiteralPath $missingConfig)) 'missing explicit input config reports IO error without creating it'
$overwriteReport=Join-Path $runRoot 'overwrite-priority.json'
$force=Run-Cli @('--config',$savedEffective,'--overwrite','--existing-file','number',
    '--output',$overrideCsv,'--report',$overwriteReport) $true
Check ($force.exit -eq 0 -and (Get-Content -LiteralPath $overwriteReport -Raw | ConvertFrom-Json).config.output.existing_file -eq 'overwrite') 'explicit --overwrite wins regardless of business option position'

$invalidArguments=@(
    @('--operators','1'), @('--parts','11'), @('--replicates','2.5'), @('--study-multiplier','0'),
    @('--seed','9223372036854775808'), @('--seed','1.5'), @('--decimal-places','-1'),
    @('--center','NaN'), @('--reference-tv-sd','0'), @('--compliance','invalid'), @('--priority','invalid'),
    @('--scale-mode','invalid'), @('--max-attempts','0'), @('--timeout-seconds','0'),
    @('--percentage-tolerance-pp','-1'), @('--ndc-tolerance','0.5'),
    @('--ev','between:20:10'), @('--av','target:10:'), @('--grr','target'), @('--pt','at_most:2:3'),
    @('--ndc','target:5:0.1'), @('--ev','none:1'), @('--run-seed','0'), @('--run-seed','-1'),
    @('--run-order','invalid'), @('--randomize-operators','yes'), @('--store-standard-order','1'),
    @('--csv-format','invalid'), @('--existing-file','invalid'), @('--filename','[UNKNOWN].csv'),
    @('--operators'), @('--seed','--parts','5'), @('--no-config=true'), @('--unknown','1'),
    @('--no-config','--config',$config), @('--init','--parts','5'), @('--output='))
$invalidIndex=0
foreach($arguments in $invalidArguments){
    $invalidPath=Join-Path $runRoot "invalid-$invalidIndex.csv"
    $badArgs=Run-Cli (@('--no-config','--output',$invalidPath)+$arguments) $true
    Check ($badArgs.exit -eq 2 -and $badArgs.stderr.Length -gt 0 -and -not (Test-Path -LiteralPath $invalidPath)) "invalid CLI options rejected before writing: $($arguments -join ' ')"
    $invalidIndex++
}
$helpFlags=@('--operators','--replicates','--trials','--parts','--study-multiplier','--lsl','--usl',
    '--compliance','--center','--priority','--scale-mode','--reference-tv-sd','--decimal-places',
    '--seed','--max-attempts','--timeout-seconds','--percentage-tolerance-pp','--ndc-tolerance',
    '--ev','--av','--grr','--pt','--ndc','--output-directory','--filename','--existing-file',
    '--csv-format','--run-order','--randomize-operators','--run-seed','--store-standard-order','--no-config')
Check (@($helpFlags | Where-Object { -not $help.stdout.Contains($_) }).Count -eq 0 -and $help.stdout.Contains('target:VALUE[:ERROR]')) 'help lists every GUI business option, aliases and constraint syntax'

# Legal JSON whitespace widens the preflight/commit interval without changing parameters.
$data = Get-Content -LiteralPath $config -Raw -Encoding UTF8 | ConvertFrom-Json
$data.generation.seed = $null
$raceConfig = Join-Path $runRoot 'race.json'
[IO.File]::WriteAllText($raceConfig, ($data | ConvertTo-Json -Depth 10) + (' ' * (20*1024*1024)), [Text.UTF8Encoding]::new($false))
foreach ($kind in 'csv','report') {
    $jobs = @()
    $shared = Join-Path $runRoot "shared.$kind"
    for ($i=0; $i -lt 4; $i++) {
        $outputPath = if ($kind -eq 'csv') { $shared } else { Join-Path $runRoot "individual-$i.csv" }
        $reportPath = if ($kind -eq 'report') { $shared } else { Join-Path $runRoot "individual-$i.json" }
        $jobs += [pscustomobject]@{job=(Start-Cli @('--config',$raceConfig,'--output',$outputPath,'--report',$reportPath));csv=$outputPath;report=$reportPath}
    }
    $finished = @($jobs | ForEach-Object { [pscustomobject]@{result=(Finish-Cli $_.job);csv=$_.csv;report=$_.report} })
    $winners = @($finished | Where-Object {$_.result.exit -eq 0})
    Check ($winners.Count -eq 1) "concurrent $kind has exactly one successful commit"
    Check (@($finished | Where-Object {$_.result.exit -notin 0,1,2}).Count -eq 0) "concurrent $kind losers return controlled errors"
    $winner = $winners[0]
    $winnerReport = Get-Content -LiteralPath $winner.report -Raw -Encoding UTF8 | ConvertFrom-Json
    $stdoutReport = $winner.result.stdout | ConvertFrom-Json
    Check ($winnerReport.seed -eq $stdoutReport.seed) "concurrent $kind report belongs to winning process"
    $rowIndex = 0
    $rows = @($winnerReport.matrix | ForEach-Object {
        $label = [char]([int][char]'A' + [int][Math]::Floor($rowIndex / $data.design.replicates))
        $rowIndex++
        $label + ',' + (($_.ForEach({ $_.ToString('F4',[Globalization.CultureInfo]::InvariantCulture) })) -join ',')
    })
    $expected = ',' + ((1..$data.design.parts) -join ',') + "`r`n" + ($rows -join "`r`n") + "`r`n"
    $actual = [IO.File]::ReadAllText($winner.csv,[Text.Encoding]::UTF8)
    Check ($actual -ceq $expected) "concurrent $kind actual CSV equals winning report matrix"
}
$data = Get-Content -LiteralPath (Join-Path $root 'examples\默认配置.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$data.specification.lsl=0; $data.specification.usl=1e-300; $data.specification.compliance='relaxed'
$data.generation.scale_mode='reference_tv_sd'; $data.generation.reference_tv_sd=1e12; $data.generation.seed=123
$overflowConfig=Join-Path $runRoot 'overflow.json'
$data | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $overflowConfig -Encoding UTF8
$protected=Join-Path $runRoot 'overflow.csv'
[IO.File]::WriteAllText($protected,'keep')
$overflow = Run-Cli @('--config',$overflowConfig,'--output',$protected,'--overwrite')
Check ($overflow.exit -eq 1 -and $overflow.stderr.Contains('P/T') -and [IO.File]::ReadAllText($protected) -eq 'keep') 'overflow reports error and preserves existing CSV'
$anovaCsv=Join-Path $runRoot 'anova.csv';$anovaReport=Join-Path $runRoot 'anova.json'
$anova=Run-Cli @('--config',$config,'--method','crossed_anova','--interaction','auto','--interaction-alpha','0.05','--seed','123','--csv-format','minitab','--run-order','all','--run-seed','321','--store-standard-order','true','--output',$anovaCsv,'--report',$anovaReport)
Check ($anova.exit-eq0) 'ANOVA CLI overrides old GUI configuration and generates data'
$anovaResult=Get-Content -LiteralPath $anovaReport -Raw -Encoding UTF8 | ConvertFrom-Json
Check ($anovaResult.config.analysis.method-eq'crossed_anova' -and $anovaResult.metrics.anova.full_table.Count-eq5) 'ANOVA report includes selected method and full table'
$readback=Join-Path $runRoot 'anova-readback.json'
$analysis=Run-Cli @('--config',$config,'--method','crossed_anova','--input',$anovaCsv,'--report',$readback)
Check ($analysis.exit-eq0) 'CLI analyzes existing randomized CSV without generation'
$analyzed=Get-Content -LiteralPath $readback -Raw -Encoding UTF8 | ConvertFrom-Json
Check ($analyzed.analysis_only -and $null-eq$analyzed.seed -and ($analyzed.metrics | ConvertTo-Json -Depth 20 -Compress)-ceq($anovaResult.metrics | ConvertTo-Json -Depth 20 -Compress)) 'analysis-only metrics equal generated report and seed is not fabricated'
$badMethod=Run-Cli @('--no-config','--method','anova')
Check ($badMethod.exit-eq2) 'invalid analysis method rejected before generation'
foreach($alpha in '0','1','-0.1','NaN'){
    $invalid=Run-Cli @('--no-config','--interaction-alpha',$alpha)
    Check ($invalid.exit-eq2) "invalid interaction alpha $alpha rejected"
}
$sameInput=Run-Cli @('--no-config','--input',$anovaCsv,'--report',$anovaCsv,'--overwrite')
Check ($sameInput.exit-eq2) 'analysis report cannot overwrite source CSV'
$mixedInput=Run-Cli @('--no-config','--input',$anovaCsv,'--output',(Join-Path $runRoot 'invalid-input-output.csv'))
Check ($mixedInput.exit-eq2) 'analysis-only input cannot silently regenerate or reexport'
$extendedReport=Join-Path $runRoot 'analysis-options.json'
$extended=Run-Cli @('--config',$config,'--method','crossed_anova','--input',$anovaCsv,'--analysis-tolerance','2','--historical-sd','0.5','--historical-mean','10','--process-variation','study','--misclassification','true','--confidence','true','--confidence-level','90','--confidence-bound','upper','--confidence-method','satterthwaite','--report',$extendedReport)
Check ($extended.exit-eq0) 'CLI accepts extended GUI analysis parameters'
$extendedResult=Get-Content -LiteralPath $extendedReport -Raw -Encoding UTF8 | ConvertFrom-Json
Check ($extendedResult.config.analysis.historical_sd-eq0.5 -and $extendedResult.config.analysis.confidence.level-eq90 -and $extendedResult.config.analysis.process_variation-eq'study' -and $extendedResult.config.analysis.confidence.method-eq'satterthwaite') 'extended CLI options override saved configuration'
Check ($extendedResult.metrics.confidence.bound-eq'upper' -and $null-eq$extendedResult.metrics.confidence.stddev.GRR.lower -and $extendedResult.metrics.confidence.stddev.GRR.upper-gt0) 'one-sided confidence report keeps only requested bound'
Check ($null-ne$extendedResult.metrics.misclassification -and [Math]::Abs($extendedResult.metrics.GRR-$analyzed.metrics.GRR)-lt1e-12) 'extra report calculations preserve measurement estimate'
foreach($badOption in @(@('--historical-sd','0'),@('--analysis-tolerance','-1'),@('--confidence-level','100'),@('--confidence-bound','invalid'),@('--confidence-method','invalid'))){
    $invalid=Run-Cli (@('--no-config')+$badOption)
    Check ($invalid.exit-eq2) "invalid extended option $($badOption -join ' ') rejected"
}
Check ((Get-FileHash -LiteralPath $config).Hash-eq$before) 'ANOVA and input analysis do not modify user configuration'
[ordered]@{passed=$true;version="$ExpectedVersion-native";checks=$checks;run_directory=$runRoot} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'cli-regression.json') -Encoding UTF8
Write-Output "$($checks.Count) CLI regression checks PASS"
