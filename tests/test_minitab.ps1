param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '../build'),
    [int]$CaseLimit = 0
)
# Optional integration test: run with Windows PowerShell -STA and an installed,
# licensed Minitab desktop. Normal CTest and runtime builds do not require COM.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'minitab_pt_policy.ps1')
$culture = [Globalization.CultureInfo]::InvariantCulture
[Threading.Thread]::CurrentThread.CurrentCulture = $culture
$root = Split-Path $PSScriptRoot -Parent
$buildRoot = (Resolve-Path -LiteralPath $BuildDirectory).Path
$cli = Join-Path $buildRoot 'Release/msa_cli.exe'
$fixturePath = Join-Path $PSScriptRoot 'fixtures/minitab_baseline.json'
$fixture = Get-Content -LiteralPath $fixturePath -Raw -Encoding UTF8 | ConvertFrom-Json
$out = Join-Path $buildRoot ('minitab-live-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $out | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
function Save-Json($Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 30) + "`n", $utf8)
}
function Require($Condition, $Message) { if (-not $Condition) { throw $Message } }
function Invoke-Cli($Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $cli
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [Text.Encoding]::UTF8
    $info.StandardErrorEncoding = [Text.Encoding]::UTF8
    $info.Arguments = (($Arguments | ForEach-Object {
        '"' + [regex]::Replace([regex]::Replace($_, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
    }) -join ' ')
    $process = [Diagnostics.Process]::Start($info)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(40000)) { $process.Kill(); throw 'CLI timed out' }
        $text = $stdout.GetAwaiter().GetResult()
        $errorText = $stderr.GetAwaiter().GetResult()
        Require ($process.ExitCode -eq 0) ('CLI: ' + $errorText)
        return $text | ConvertFrom-Json
    } finally { $process.Dispose() }
}
function Invoke-Minitab($Command, $Worksheet) {
    if ($null -eq $Worksheet) { $script:project.ExecuteCommand($Command) }
    else { $script:project.ExecuteCommand($Command, $Worksheet) }
    Require ([string]::IsNullOrWhiteSpace($script:mtb.LastError)) ('Minitab: ' + $script:mtb.LastError)
}
function Read-Numeric($Text) { return [double]::Parse($Text, $culture) }
$maxErrors = @{}
function Compare-Number($Name, [double]$Actual, [double]$Expected) {
    Require (-not [double]::IsNaN($Actual) -and -not [double]::IsInfinity($Actual)) "$Name is not finite"
    $errorValue = [Math]::Abs($Actual - $Expected)
    if (-not $maxErrors.ContainsKey($Name)) { $maxErrors[$Name] = 0.0 }
    $maxErrors[$Name] = [Math]::Max($maxErrors[$Name], $errorValue)
    Require ($errorValue -le 1e-11 + 1e-9 * [Math]::Abs($Expected)) "$Name mismatch: native=$Actual minitab=$Expected error=$errorValue"
}

$cases = [Collections.Generic.List[object]]::new()
foreach ($entry in $fixture.cases) {
    $cfg = $entry.config | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $cfg.generation.seed = [long]$cfg.generation.seed + 100000
    $cases.Add([pscustomobject]@{id=$entry.id;config=$cfg;scenario=($entry.id -split '_r[23]_')[1]})
}
# Exercise coarse and fine quantization with suitable scales and all 24 designs.
foreach ($entry in @($fixture.cases | Where-Object {$_.id -match '_all_seed1$'})) {
    foreach ($dp in 0,10) {
        $cfg = $entry.config | ConvertTo-Json -Depth 20 | ConvertFrom-Json
        $cfg.specification = [pscustomobject]@{lsl=$null;usl=$null;compliance='relaxed'}
        $cfg.generation.center = if ($dp -eq 0) { 1000 } else { 0 }
        $cfg.generation.reference_tv_sd = if ($dp -eq 0) { 100 } else { 0.01 }
        $cfg.generation.scale_mode = 'reference_tv_sd'
        $cfg.generation.decimal_places = $dp
        $cfg.generation.seed = -12345 - $dp
        $cfg.targets = [pscustomobject]@{EV_pct=$null;AV_pct=$null;GRR_pct=$null;PT_pct=$null;ndc=$null}
        $cases.Add([pscustomobject]@{id=($entry.id -replace '_all_seed1$', "_precision$dp");config=$cfg;scenario="precision$dp"})
    }
}
if ($CaseLimit -gt 0) { $selected = @($cases | Select-Object -First $CaseLimit) } else { $selected = @($cases.ToArray()) }
$formats = @('default','minitab','spss','jmp19','origin','matlab')
$modes = @('standard','all','within_operator')
$results = [Collections.Generic.List[object]]::new()
$mtb = $null
$stopwatch = [Diagnostics.Stopwatch]::StartNew()
try {
    $mtb = New-Object -ComObject Mtb.Application
    $mtb.UserInterface.Visible = $false
    $mtb.UserInterface.DisplayAlerts = $false
    $minitabVersion = $mtb.Version
    $minitabPath = Join-Path $mtb.AppPath 'Mtb.exe'
    if (-not (Test-Path -LiteralPath $minitabPath)) { $minitabPath = 'C:/Program Files/Minitab/Minitab 22/Mtb.exe' }
    for ($index=0; $index -lt $selected.Count; $index++) {
        $case = $selected[$index]
        $caseRoot = Join-Path $out $case.id
        New-Item -ItemType Directory -Path $caseRoot | Out-Null
        try {
            $cfg = $case.config
            $format = $formats[$index % 6]
            $mode = $modes[[int][Math]::Floor($index / 6) % 3]
            $store = [bool]([int][Math]::Floor($index / 3) % 2)
            $cfg.output | Add-Member -NotePropertyName format -NotePropertyValue $format -Force
            $cfg.output | Add-Member -NotePropertyName run_order -NotePropertyValue ([pscustomobject]@{
                mode=$mode;seed=321;randomize_operators=[bool]([int][Math]::Floor($index / 18) % 2);store_standard_order=$store
            }) -Force
            $configPath = Join-Path $caseRoot 'config.json'
            $csvPath = Join-Path $caseRoot 'data.csv'
            $reportPath = Join-Path $caseRoot 'native.json'
            Save-Json $configPath $cfg
            $cliResult = Invoke-Cli @('--config',$configPath,'--output',$csvPath,'--report',$reportPath)
            $native = Get-Content -LiteralPath $reportPath -Raw -Encoding UTF8 | ConvertFrom-Json
            $operators = [int]$cfg.design.operators
            $trials = [int]$cfg.design.replicates
            $parts = [int]$cfg.design.parts
            $count = $operators * $trials * $parts
            $mtb.New()
            $project = $mtb.ActiveProject
            Invoke-Minitab ('WOPEN "'+$csvPath+'"; FTYPE; CSV; FIELD; COMMA; DECSEP; PERIOD; SHEET 1; VNAMES 1; FIRST 2.') $null
            $sheet = $project.ActiveWorksheet
            $importPath = Join-Path $caseRoot 'minitab-import.txt'
            if ($format -eq 'default') {
                Invoke-Minitab ('WRITE C1-C'+($parts+1)+'; FILE "'+$importPath+'"; FORMAT (A1,'+$parts+'E26.17).') $sheet
                $rows = @(Get-Content -LiteralPath $importPath)
                Require ($rows.Count -eq $operators*$trials) 'Wide import row count'
                for ($row=0; $row -lt $rows.Count; $row++) {
                    $cells = $rows[$row].Trim() -split '\s+'
                    Require ($cells.Count -eq $parts+1 -and $cells[0] -ceq [string][char](65+[int][Math]::Floor($row/$trials))) 'Wide imported column count/operator'
                    for ($part=0; $part -lt $parts; $part++) {
                        Require ((Read-Numeric $cells[$part+1]) -eq [double]$native.matrix[$row][$part]) 'Wide imported measurement precision/mapping'
                    }
                }
                Invoke-Minitab ('STACK C2-C'+($parts+1)+' C20; SUBSCRIPTS C22.') $sheet
                Invoke-Minitab ('STACK '+((@('C1')*$parts) -join ' ')+' C21.') $sheet
                $analysisColumns = 'PARTS C22; OPERS C21; RESPONSE C20; '
            } else {
                $offset = if ($store) { 1 } else { 0 }
                $inputFormat = if ($store) { 'F8.0,1X,A1,1X,F8.0,1X,F8.0,1X,E26.17' } else { 'A1,1X,F8.0,1X,F8.0,1X,E26.17' }
                Invoke-Minitab ('WRITE C1-C'+(4+$offset)+'; FILE "'+$importPath+'"; FORMAT ('+$inputFormat+').') $sheet
                $rows = @(Get-Content -LiteralPath $importPath)
                $sourceRows = @(Import-Csv -LiteralPath $csvPath -Encoding UTF8)
                Require ($rows.Count -eq $count -and $sourceRows.Count -eq $count) 'Long import row count'
                $seen = @{}
                for ($row=0; $row -lt $rows.Count; $row++) {
                    $cells = $rows[$row].Trim() -split '\s+'
                    Require ($cells.Count -eq 4+$offset) 'Long import column count'
                    $op = [int][char]$cells[$offset] - 65
                    $part = [int](Read-Numeric $cells[$offset+1])
                    $trial = [int](Read-Numeric $cells[$offset+2])
                    Require ($op -ge 0 -and $op -lt $operators -and $part -ge 1 -and $part -le $parts -and $trial -ge 1 -and $trial -le $trials) 'Long import identifiers'
                    $id = (($trial-1)*$parts+$part-1)*$operators+$op+1
                    Require (-not $seen.ContainsKey($id)) 'Long duplicate record'
                    $seen[$id] = $true
                    Require ($cells[$offset] -ceq $sourceRows[$row].Operator -and $part -eq [int]$sourceRows[$row].Part -and $trial -eq [int]$sourceRows[$row].Trial) 'Minitab reordered CSV rows or changed Trial'
                    # Windows PowerShell may deserialize JSON numbers as Decimal;
                    # the application and Minitab use IEEE double. Compare doubles
                    # exactly, without introducing a tolerance or losing CSV digits.
                    Require ((Read-Numeric $cells[$offset+3]) -eq [double]$native.matrix[$op*$trials+$trial-1][$part-1]) 'Long imported measurement precision/mapping'
                    if ($store) { Require ((Read-Numeric $cells[0]) -eq $id -and $id -eq [int]$sourceRows[$row].StdOrder) 'Imported StdOrder identity' }
                }
                Require ($seen.Count -eq $count) 'Missing measurement records'
                $analysisColumns = 'PARTS C'+(2+$offset)+'; OPERS C'+(1+$offset)+'; RESPONSE C'+(4+$offset)+'; '
            }
            $bothSpecs = $null -ne $cfg.specification.lsl -and $null -ne $cfg.specification.usl
            $command = 'GAGERR; '+$analysisColumns+'XBAR; STUDYVAR '+$cfg.analysis.study_multiplier+'; SSTDEV C30; SDISTINCT C31; SSTUDYVAR C32 C33'
            if ($bothSpecs) { $command += '; TOLER '+([double]$cfg.specification.usl-[double]$cfg.specification.lsl)+'; STOLERANCE C34' }
            $command += '.'
            [IO.File]::WriteAllText((Join-Path $caseRoot 'analysis.mtb'), $command, $utf8)
            Invoke-Minitab $command $sheet
            $nativePath = Join-Path $caseRoot 'minitab-statistics.txt'
            $last = if ($bothSpecs) { 34 } else { 33 }
            $width = if ($bothSpecs) { 5 } else { 4 }
            Invoke-Minitab ('WRITE C30-C'+$last+'; FILE "'+$nativePath+'"; FORMAT ('+$width+'E26.17).') $sheet
            $numericRows = @(Get-Content -LiteralPath $nativePath | ForEach-Object { ,($_.Trim() -split '\s+') })
            Require ($numericRows.Count -eq 5) 'Minitab statistics row count'
            $minitab = [ordered]@{sd=@();studyvar=@();percent=@();ndc=(Read-Numeric $numericRows[0][1]);tolerance=@()}
            $keys = @('GRR','EV','AV','PV','TV')
            for ($k=0; $k -lt 5; $k++) {
                $sd = Read-Numeric $numericRows[$k][0]
                $sv = Read-Numeric $numericRows[$k][2]
                $percent = Read-Numeric $numericRows[$k][3]
                $minitab.sd += $sd; $minitab.studyvar += $sv; $minitab.percent += $percent
                Compare-Number $keys[$k] $native.metrics.($keys[$k]) $sd
                Compare-Number ($keys[$k]+'_studyvar') ([double]$native.metrics.($keys[$k])*$cfg.analysis.study_multiplier) $sv
                if ($k -lt 3) { Compare-Number ($keys[$k]+'_pct') $native.metrics.($keys[$k]+'_pct') $percent }
                if ($bothSpecs) { $minitab.tolerance += (Read-Numeric $numericRows[$k][4]) }
            }
            Require ($native.metrics.ndc -eq $minitab.ndc) 'ndc must match exactly'
            $externalPt = if ($bothSpecs) { $minitab.tolerance[0] } else { $null }
            $ptComparison = Test-MinitabPtComparison -Config $cfg -NativeMetrics $native.metrics -MinitabTolerance $externalPt
            if ($ptComparison.compared) { Compare-Number 'PT_pct' $native.metrics.PT_pct $externalPt }
            $minitab.pt_comparison = $ptComparison
            Save-Json (Join-Path $caseRoot 'minitab.json') $minitab
            $results.Add([pscustomobject]@{id=$case.id;scenario=$case.scenario;passed=$true;format=$format;mode=$mode;store_standard_order=$store;records=$count;native=$native.metrics;minitab=$minitab;pt_comparison=$ptComparison;csv_sha256=(Get-FileHash -LiteralPath $csvPath -Algorithm SHA256).Hash})
        } catch {
            $results.Add([pscustomobject]@{id=$case.id;scenario=$case.scenario;passed=$false;error=$_.Exception.Message})
            Write-Output ('FAIL '+$case.id+': '+$_.Exception.Message)
        }
        if (($index+1) % 12 -eq 0) { Write-Output ('Minitab live comparison '+($index+1)+'/'+$selected.Count) }
    }
} finally {
    if ($mtb) {
        # This instance contains only test data. Suppress unsaved-project prompts
        # through Quit, which destroys this private automation instance.
        $mtb.UserInterface.DisplayAlerts = $false
        $mtb.Quit()
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($mtb)
    }
}
$failures = @($results | Where-Object {-not $_.passed})
$summary = [ordered]@{
    passed=($failures.Count -eq 0 -and $results.Count -eq $selected.Count)
    utc=[datetime]::UtcNow.ToString('o');elapsed_seconds=$stopwatch.Elapsed.TotalSeconds
    minitab_version=$minitabVersion;method='Crossed Gage R&R / Xbar-R; study variation from each configuration; bilateral tolerance only'
    expected_cases=$selected.Count;completed_cases=$results.Count;failed_cases=$failures.Count
    designs=@($selected | ForEach-Object { $_.config.design | ConvertTo-Json -Compress } | Select-Object -Unique).Count
    formats=@($results | Where-Object passed | Group-Object format | ForEach-Object { @{format=$_.Name;count=$_.Count} })
    tolerance='abs(actual - Minitab) <= 1e-11 + 1e-9 * abs(Minitab); ndc exact; imported measurement values exact'
    maximum_absolute_errors=$maxErrors
    pt_comparisons=@($results | Where-Object passed | Group-Object { $_.pt_comparison.status } | ForEach-Object { @{status=$_.Name;count=$_.Count} })
    artifacts=@{cli_sha256=(Get-FileHash -LiteralPath $cli -Algorithm SHA256).Hash;gui_sha256=(Get-FileHash -LiteralPath (Join-Path $buildRoot 'Release/msa_generator.exe') -Algorithm SHA256).Hash;baseline_sha256=(Get-FileHash -LiteralPath $fixturePath -Algorithm SHA256).Hash;minitab_sha256=(Get-FileHash -LiteralPath $minitabPath -Algorithm SHA256).Hash}
    cases=$results.ToArray()
}
Save-Json (Join-Path $out 'comparison.json') $summary
[IO.File]::WriteAllText((Join-Path $buildRoot 'latest-minitab-comparison.txt'), $out, $utf8)
Write-Output ('Minitab live: '+$results.Count+' cases, '+$failures.Count+' failures; '+$out)
if (-not $summary.passed) { exit 1 }
