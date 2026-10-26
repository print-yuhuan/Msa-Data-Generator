param([Parameter(Mandatory)][string]$Manifest)
$ErrorActionPreference='Stop'
$cases=(Get-Content -LiteralPath $Manifest -Raw -Encoding utf8 | ConvertFrom-Json).cases
$script=[Text.StringBuilder]::new()
foreach($case in $cases){
    $path=$case.csv.Replace('\','/');$folder=$case.folder.Replace('\','/')
    [void]$script.AppendLine(('dt=Open("'+$path+'",Invisible); Column(dt,"Part")<<Set Modeling Type("Nominal"); Column(dt,"Operator")<<Set Modeling Type("Nominal"); Column(dt,"Measurement")<<Format("Scientific",32,17); dt<<Save("'+$folder+'/jmp-import.csv");'))
    foreach($model in 'full','pooled'){
        $effects=if($model-eq'full'){':Part & Random,:Operator & Random,:Part*:Operator & Random'}else{':Part & Random,:Operator & Random'}
        [void]$script.AppendLine(('fit=dt<<Fit Model(Y(:Measurement),Effects('+$effects+'),Personality("Standard Least Squares"),Method("EMS"),Run); rr=Report(fit);'))
        foreach($item in @(@('variance','Fit EMS Var Comp'),@('tests','Fit EMS Tests'))){
            $template=@'
boxes=rr<<XPath("//OutlineBox[@helpKey=\!"KEY\!"]//TableBox"); tmp=boxes[1]<<Make Into Data Table; For(k=2,k<=N Cols(tmp),k++,Column(tmp,k)<<Format("Scientific",32,17));tmp<<Save("DEST");Close(tmp,NoSave);
'@
            [void]$script.AppendLine($template.Replace('KEY',$item[1]).Replace('DEST',($folder+'/jmp-'+$model+'-'+$item[0]+'.csv')))
        }
        [void]$script.AppendLine('fit<<Close Window;')
    }
    [void]$script.AppendLine('Close(dt,NoSave);')
}
$jsl=Join-Path (Split-Path $Manifest -Parent) 'run-jmp.jsl'
[IO.File]::WriteAllText($jsl,$script.ToString(),[Text.UTF8Encoding]::new($false))
$app=New-Object -ComObject JMP.Application
try{
    $app.RunJSLFile($jsl)
    if($app.HasRunCommandErrorString()){throw $app.GetRunCommandErrorString()}
    foreach($case in $cases){foreach($model in 'full','pooled'){foreach($kind in 'variance','tests'){if(-not(Test-Path -LiteralPath (Join-Path $case.folder ('jmp-'+$model+'-'+$kind+'.csv')))){throw 'JMP缺少原生结果表'}}}}
    Write-Output ('JMP已完成 '+$cases.Count+' 组全模型和合并模型。')
}finally{$app.Quit()}
