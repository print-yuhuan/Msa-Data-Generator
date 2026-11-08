param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '../build'),
    [string]$Config = (Join-Path $PSScriptRoot '../examples/示例配置.json')
)
# Run with Windows PowerShell -STA and a licensed Minitab desktop installation.
$ErrorActionPreference = 'Stop'
$culture = [Globalization.CultureInfo]::InvariantCulture
[Threading.Thread]::CurrentThread.CurrentCulture = $culture
$buildRoot = (Resolve-Path -LiteralPath $BuildDirectory).Path
$out = Join-Path $buildRoot ('minitab-same-study-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$utf8 = [Text.UTF8Encoding]::new($false)
function Require($Value, $Message) { if (-not $Value) { throw $Message } }
function Save-Json($Path, $Value) { [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 30) + "`n", $utf8) }
function Number($Text) { return [double]::Parse($Text, $culture) }
function Near([double]$Actual, [double]$Expected) {
    return -not [double]::IsNaN($Actual) -and -not [double]::IsInfinity($Actual) -and
        [Math]::Abs($Actual-$Expected) -le 1e-11+1e-9*[Math]::Abs($Expected)
}
function Invoke-Minitab($Command, $Worksheet) {
    if ($null -eq $Worksheet) { $script:project.ExecuteCommand($Command) }
    else { $script:project.ExecuteCommand($Command, $Worksheet) }
    Require ([string]::IsNullOrWhiteSpace($script:mtb.LastError)) ('Minitab: '+$script:mtb.LastError)
}
$exportTest = Join-Path $buildRoot 'Release/msa_export_tests.exe'
& $exportTest --same-study (Resolve-Path -LiteralPath $Config).Path $out
Require ($LASTEXITCODE -eq 0) 'Same-study export failed'
$study = Get-Content -LiteralPath (Join-Path $out 'study.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$exports = Get-Content -LiteralPath (Join-Path $out 'exports.json') -Raw -Encoding UTF8 | ConvertFrom-Json
Require ($exports.generation_calls -eq 1 -and $exports.source_matrix_unchanged -and $exports.native_metrics_unchanged) 'Study regenerated or changed'
$operators = [int]$study.config.design.operators
$trials = [int]$study.config.design.replicates
$parts = [int]$study.config.design.parts
$count = $operators*$trials*$parts
$multiplier = [double]$study.config.analysis.study_multiplier
$spec = $study.config.specification
$bothSpecs = $null -ne $spec.lsl -and $null -ne $spec.usl
$components = @('GRR','EV','AV','PV','TV')
$programMetrics = [ordered]@{}
foreach ($key in $components) {
    $sd = [double]$study.metrics.$key
    $programMetrics[$key] = $sd
    $programMetrics[$key+'_studyvar'] = $sd*$multiplier
    $programMetrics[$key+'_pct'] = if ($key -in 'GRR','EV','AV') { [double]$study.metrics.($key+'_pct') } else { 100*$sd/[double]$study.metrics.TV }
    if ($bothSpecs) {
        $programMetrics[$key+'_tolerance_pct'] = if ($key -eq 'GRR') { [double]$study.metrics.PT_pct } else { 100*$sd*$multiplier/([double]$spec.usl-[double]$spec.lsl) }
    }
}
$programMetrics['ndc'] = [int]$study.metrics.ndc
$results = [ordered]@{}
$recordChecks = [Collections.Generic.List[object]]::new()
$canonicalHash = $null
$mtb = $null
try {
    $mtb = New-Object -ComObject Mtb.Application
    $mtb.UserInterface.Visible = $false
    $mtb.UserInterface.DisplayAlerts = $false
    $version = $mtb.Version
    $minitabExe = Join-Path $mtb.AppPath 'Mtb.exe'
    if (-not (Test-Path -LiteralPath $minitabExe)) { $minitabExe = 'C:/Program Files/Minitab/Minitab 22/Mtb.exe' }
    foreach ($file in $exports.files) {
        $mode = $file.mode
        $path = Join-Path $out $file.file
        $source = @(Import-Csv -LiteralPath $path -Encoding UTF8)
        Require ($source.Count -eq $count) "$mode source count"
        $bytes = [IO.File]::ReadAllBytes($path)
        Require (-not ($bytes[0] -eq 239 -and $bytes[1] -eq 187 -and $bytes[2] -eq 191)) "$mode unexpected BOM"
        $text = [IO.File]::ReadAllText($path, $utf8)
        Require ($text.StartsWith("StdOrder,Operator,Part,Trial,Measurement`r`n") -and $text -notmatch '(?<!\r)\n') "$mode header or CRLF"
        $canonical = @($source | Sort-Object {[int]$_.StdOrder} | ForEach-Object {
            $_.StdOrder+','+$_.Operator+','+$_.Part+','+$_.Trial+','+$_.Measurement
        }) -join "`r`n"
        $hasher = [Security.Cryptography.SHA256]::Create()
        try { $hash = [BitConverter]::ToString($hasher.ComputeHash($utf8.GetBytes($canonical))).Replace('-','') } finally { $hasher.Dispose() }
        if ($null -eq $canonicalHash) { $canonicalHash = $hash }
        Require ($hash -eq $canonicalHash) "$mode canonical records differ from the same study"
        $mtb.New()
        $project = $mtb.ActiveProject
        Invoke-Minitab ('WOPEN "'+$path+'"; FTYPE; CSV; FIELD; COMMA; DECSEP; PERIOD; SHEET 1; VNAMES 1; FIRST 2.') $null
        $sheet = $project.ActiveWorksheet
        $importPath = Join-Path $out ($mode+'-minitab-import.txt')
        Invoke-Minitab ('WRITE C1-C5; FILE "'+$importPath+'"; FORMAT (F8.0,1X,A1,1X,F8.0,1X,F8.0,1X,E26.17).') $sheet
        $rows = @(Get-Content -LiteralPath $importPath)
        Require ($rows.Count -eq $count) "$mode imported count"
        $seen = @{}
        $order = [Collections.Generic.List[int]]::new()
        for ($row=0; $row -lt $count; $row++) {
            $cells = $rows[$row].Trim() -split '\s+'
            Require ($cells.Count -eq 5) "$mode columns"
            $std = [int](Number $cells[0])
            $op = [int][char]$cells[1]-65
            $part = [int](Number $cells[2])
            $trial = [int](Number $cells[3])
            Require ($op -ge 0 -and $op -lt $operators -and $part -ge 1 -and $part -le $parts -and $trial -ge 1 -and $trial -le $trials) "$mode identifiers"
            $expected = (($trial-1)*$parts+$part-1)*$operators+$op+1
            Require ($std -eq $expected -and -not $seen.ContainsKey($std)) "$mode StdOrder identity or duplicate"
            $seen[$std] = $true
            $order.Add($std)
            Require ($std -eq [int]$source[$row].StdOrder -and $cells[1] -ceq $source[$row].Operator -and $part -eq [int]$source[$row].Part -and $trial -eq [int]$source[$row].Trial) "$mode Minitab changed row order or identifiers"
            Require ($std -eq $file.standard_order_ids[$row]) "$mode native export manifest order"
            $value = [double]$study.matrix[$op*$trials+$trial-1][$part-1]
            Require ((Number $cells[4]) -eq $value) "$mode changed imported measurement double"
            Require ($source[$row].Measurement -ceq $value.ToString('F'+$study.config.generation.decimal_places,$culture)) "$mode decimal precision"
        }
        Require ($seen.Count -eq $count) "$mode lost records"
        foreach ($trial in 1..$trials) { Require (@($source | Where-Object { [int]$_.Trial -eq $trial }).Count -eq $operators*$parts) "$mode Trial group count" }
        if ($mode -eq 'standard') {
            for ($i=0; $i -lt $count; $i++) { Require ($order[$i] -eq $i+1) 'Standard sequence must be Trial-Part-Operator' }
        } else {
            Require (($order.ToArray() -join ',') -ne ((1..$count) -join ',')) "$mode did not change order"
        }
        $operatorOrder = @()
        if ($mode -eq 'within_operator') {
            for ($trial=1; $trial -le $trials; $trial++) {
                for ($block=0; $block -lt $operators; $block++) {
                    $start = (($trial-1)*$operators+$block)*$parts
                    if ($trial -eq 1) { $operatorOrder += $source[$start].Operator }
                    $partSet = @{}
                    for ($i=0; $i -lt $parts; $i++) {
                        $record = $source[$start+$i]
                        Require ([int]$record.Trial -eq $trial -and $record.Operator -ceq $operatorOrder[$block]) 'Within-operator block or persistent operator order'
                        $partSet[[int]$record.Part] = $true
                    }
                    Require ($partSet.Count -eq $parts) 'Within-operator part permutation'
                }
            }
        }
        $command = 'GAGERR; PARTS C3; OPERS C2; RESPONSE C5; XBAR; STUDYVAR '+$multiplier+'; SSTDEV C30; SDISTINCT C31; SSTUDYVAR C32 C33'
        if ($bothSpecs) { $command += '; TOLER '+([double]$spec.usl-[double]$spec.lsl)+'; STOLERANCE C34' }
        $command += '.'
        [IO.File]::WriteAllText((Join-Path $out ($mode+'.mtb')), $command, $utf8)
        Invoke-Minitab $command $sheet
        $statsPath = Join-Path $out ($mode+'-minitab-statistics.txt')
        $last = if ($bothSpecs) { 34 } else { 33 }
        $width = if ($bothSpecs) { 5 } else { 4 }
        Invoke-Minitab ('WRITE C30-C'+$last+'; FILE "'+$statsPath+'"; FORMAT ('+$width+'E26.17).') $sheet
        $stats = @(Get-Content -LiteralPath $statsPath | ForEach-Object { ,($_.Trim() -split '\s+') })
        Require ($stats.Count -eq 5) 'Minitab result row count'
        $actual = [ordered]@{}
        for ($i=0; $i -lt 5; $i++) {
            $key = $components[$i]
            $actual[$key] = Number $stats[$i][0]
            $actual[$key+'_studyvar'] = Number $stats[$i][2]
            $actual[$key+'_pct'] = Number $stats[$i][3]
            if ($bothSpecs) { $actual[$key+'_tolerance_pct'] = Number $stats[$i][4] }
        }
        $actual['ndc'] = [int](Number $stats[0][1])
        foreach ($key in $programMetrics.Keys) {
            if ($key -eq 'ndc') { Require ($actual[$key] -eq $programMetrics[$key]) 'ndc must match exactly' }
            else { Require (Near $programMetrics[$key] $actual[$key]) ($mode+' metric '+$key) }
        }
        $results[$mode] = $actual
        $recordChecks.Add([ordered]@{mode=$mode;rows=$count;missing=0;duplicates=0;trial_mapping_valid=$true;stdorder_is_1_to_n=$true;stdorder_identity_valid=$true;measurement_double_exact=$true;decimal_places_valid=$true;minitab_preserved_csv_order=$true;canonical_records_sha256=$hash;operator_order=$operatorOrder;first_15_stdorder=@($order.ToArray() | Select-Object -First 15);csv_sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash})
        Write-Output ('PASS '+$mode+': '+$count+' records, Trial/StdOrder/values exact, all Minitab metrics match')
    }
} finally {
    if ($mtb) { $mtb.UserInterface.DisplayAlerts=$false; $mtb.Quit(); [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($mtb) }
}
$comparison = foreach ($key in $programMetrics.Keys) {
    $values = @($results.standard[$key], $results.all[$key], $results.within_operator[$key])
    $delta = ($values | Measure-Object -Maximum).Maximum - ($values | Measure-Object -Minimum).Minimum
    foreach ($value in $values) {
        if ($key -eq 'ndc') { Require ($value -eq $results.standard[$key]) 'Cross-order ndc mismatch' }
        else { Require (Near $value $results.standard[$key]) ('Cross-order mismatch: '+$key) }
    }
    [pscustomobject]@{metric=$key;program=$programMetrics[$key];standard=$values[0];all=$values[1];within_operator=$values[2];minitab_max_spread=$delta;max_abs_error_vs_program=(@($values | ForEach-Object {[Math]::Abs($_-$programMetrics[$key])}) | Measure-Object -Maximum).Maximum}
}
$comparison | Export-Csv -LiteralPath (Join-Path $out 'metrics-comparison.csv') -NoTypeInformation -Encoding UTF8
$report = [ordered]@{passed=$true;utc=[datetime]::UtcNow.ToString('o');minitab_version=$version;method='Crossed Gage R&R Xbar-R';generation_calls=1;generation_seed=$study.seed;run_seed=321;design=$study.config.design;records_per_file=$count;study_multiplier=$multiplier;specification=$spec;source_matrix_unchanged=$true;native_metrics_unchanged=$true;tolerance='abs(program-Minitab) <= 1e-11 + 1e-9*abs(Minitab); ndc and imported measurement doubles exact';records=$recordChecks.ToArray();metrics=@($comparison);artifacts=@{export_tests_sha256=(Get-FileHash -LiteralPath $exportTest -Algorithm SHA256).Hash;cli_sha256=(Get-FileHash -LiteralPath (Join-Path $buildRoot 'Release/msa_cli.exe') -Algorithm SHA256).Hash;minitab_sha256=(Get-FileHash -LiteralPath $minitabExe -Algorithm SHA256).Hash;study_sha256=(Get-FileHash -LiteralPath (Join-Path $out 'study.json') -Algorithm SHA256).Hash}}
Save-Json (Join-Path $out 'comparison.json') $report
[IO.File]::WriteAllText((Join-Path $buildRoot 'latest-minitab-same-study.txt'), $out, $utf8)
Write-Output ('Same-study three-order Minitab comparison PASS: '+$out)
