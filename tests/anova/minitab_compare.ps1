param([Parameter(Mandatory)][string]$Manifest)
$ErrorActionPreference='Stop'
[Threading.Thread]::CurrentThread.CurrentCulture=[Globalization.CultureInfo]::InvariantCulture
$cases=(Get-Content -LiteralPath $Manifest -Raw -Encoding utf8 | ConvertFrom-Json).cases
$app=New-Object -ComObject Mtb.Application
try {
    $app.UserInterface.Visible=$false
    $app.UserInterface.DisplayAlerts=$false
    foreach($case in $cases){
        $app.New(); $project=$app.ActiveProject
        $project.ExecuteCommand(('WOPEN "'+$case.csv+'"; FTYPE; CSV; FIELD; COMMA; DECSEP; PERIOD; SHEET 1; VNAMES 1; FIRST 2.'))
        if($app.LastError){throw $app.LastError}
        $sheet=$project.ActiveWorksheet
        $import=Join-Path $case.folder 'minitab-import.txt'
        $project.ExecuteCommand(('WRITE C1-C5; FILE "'+$import+'"; FORMAT (F8.0,1X,A1,1X,F8.0,1X,F8.0,1X,E26.17).'),$sheet)
        if($app.LastError){throw $app.LastError}
        $source=@(Import-Csv -LiteralPath $case.csv)
        $imported=@(Get-Content -LiteralPath $import)
        if($source.Count-ne$imported.Count){throw 'Minitab导入记录数错误'}
        for($i=0;$i-lt$source.Count;$i++){
            $cells=$imported[$i].Trim()-split'\s+'
            if([int]$cells[0]-ne[int]$source[$i].StdOrder -or $cells[1]-cne$source[$i].Operator -or [int]$cells[2]-ne[int]$source[$i].Part -or [int]$cells[3]-ne[int]$source[$i].Trial -or [double]$cells[4]-ne[double]$source[$i].Measurement){throw 'Minitab导入对应关系或数值精度错误'}
        }
        $cfg=Get-Content -LiteralPath $case.config -Raw | ConvertFrom-Json
        $command='GAGERR; PARTS C3; OPERS C2; RESPONSE C5; STUDYVAR '+$cfg.analysis.study_multiplier+'; TOLER '+([double]$cfg.specification.usl-[double]$cfg.specification.lsl)+'; SSTDEV C30; SDISTINCT C31; SSTUDYVAR C32 C33.'
        [IO.File]::WriteAllText((Join-Path $case.folder 'analysis.mtb'),$command,[Text.UTF8Encoding]::new($false))
        $project.ExecuteCommand($command,$sheet)
        if($app.LastError){throw $app.LastError}
        $stats=Join-Path $case.folder 'minitab-statistics.txt'
        $project.ExecuteCommand(('WRITE C30-C33; FILE "'+$stats+'"; FORMAT (4E26.17).'),$sheet)
        if($app.LastError){throw $app.LastError}
        $rows=@(Get-Content -LiteralPath $stats | ForEach-Object {,($_.Trim()-split'\s+')})
        $names=if($rows.Count-eq7){@('GRR','EV','AV','Operator','OperatorPart','PV','TV')}elseif($rows.Count-eq6){@('GRR','EV','AV','Operator','PV','TV')}else{throw 'Minitab统计表来源数错误'}
        $sd=[ordered]@{}; $sv=[ordered]@{}; $percent=[ordered]@{}
        for($i=0;$i-lt$names.Count;$i++){$sd[$names[$i]]=[double]$rows[$i][0];$sv[$names[$i]]=[double]$rows[$i][2];$percent[$names[$i]]=[double]$rows[$i][3]}
        $result=[ordered]@{version=$app.Version;records=$source.Count;import_exact=$true;interaction_included=($rows.Count-eq7);stddev=$sd;study_variation=$sv;study_variation_pct=$percent;ndc=[double]$rows[0][1]}
        $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $case.folder 'minitab.json') -Encoding utf8
        Write-Output ('Minitab完成 '+$case.id)
    }
} finally {$app.UserInterface.DisplayAlerts=$false;$app.Quit()}
