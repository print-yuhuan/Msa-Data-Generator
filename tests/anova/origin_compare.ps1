param([Parameter(Mandatory)][string]$Manifest)
$ErrorActionPreference='Stop'
if((Get-Item -LiteralPath 'C:/Program Files/OriginLab/Origin2026b/ocMath.dll').VersionInfo.FileVersion-ne'10.3.5'){throw '此Origin原生接口适配器仅验证了Origin2026b/10.3.5'}
$cases=(Get-Content -LiteralPath $Manifest -Raw -Encoding utf8 | ConvertFrom-Json).cases
$app=New-Object -ComObject Origin.ApplicationSI
try{
    foreach($case in $cases){
        $rows=@(Import-Csv -LiteralPath $case.csv)
        $data=New-Object 'double[,]' $rows.Count,5
        for($i=0;$i-lt$rows.Count;$i++){
            $data[$i,0]=[int]$rows[$i].StdOrder;$data[$i,1]=[int][char]$rows[$i].Operator-64
            $data[$i,2]=[int]$rows[$i].Part;$data[$i,3]=[int]$rows[$i].Trial;$data[$i,4]=[double]$rows[$i].Measurement
        }
        [void]$app.NewProject()
        if(-not$app.Execute('newbook name:=MsaData sheet:=1;')){throw 'Origin新建工作表失败'}
        if(-not$app.PutWorksheet('[MsaData]Sheet1',$data,0,0)){throw 'Origin工作表写入失败'}
        $restored=$app.GetWorksheet('[MsaData]Sheet1',0,0,($rows.Count-1),4,0)
        if($restored.GetLength(0)-ne$rows.Count -or $restored.GetLength(1)-ne5){throw 'Origin回读维度错误'}
        $rowBase=$restored.GetLowerBound(0);$colBase=$restored.GetLowerBound(1)
        $back=[Collections.Generic.List[object]]::new()
        for($i=0;$i-lt$rows.Count;$i++){
            $record=@()
            for($j=0;$j-lt5;$j++){if($restored[($i+$rowBase),($j+$colBase)]-ne$data[$i,$j]){throw 'Origin工作表回读精度或对应关系错误'};$record+=[double]$restored[($i+$rowBase),($j+$colBase)]}
            $back.Add($record)
        }
        [ordered]@{data=$back.ToArray();records=$rows.Count;import_exact=$true} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $case.folder 'origin-import.json') -Encoding utf8
    }
    $code=(Join-Path $PSScriptRoot 'origin_engine.py').Replace('\','/')
    [void]$app.Execute(('string msa_manifest$="'+$Manifest.Replace('\','/')+'"; msa_python_ok=0; run -pyf "'+$code+'";'))
    if($app.Evaluate('msa_python_ok')-ne1){throw 'Origin内置Python运行原生计算接口失败'}
    Write-Output ('Origin已完成 '+$cases.Count+' 组工作表回读及原生ANOVA。')
}finally{$app.Exit()}
