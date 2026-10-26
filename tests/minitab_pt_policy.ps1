# 批量实测当前只向 Minitab 传入双侧容差；明确区分已比较、未比较与不可计算。
function Test-MinitabPtComparison {
    param(
        [Parameter(Mandatory)]$Config,
        [Parameter(Mandatory)]$NativeMetrics,
        $MinitabTolerance = $null
    )
    $both = $null -ne $Config.specification.lsl -and $null -ne $Config.specification.usl
    $any = $null -ne $Config.specification.lsl -or $null -ne $Config.specification.usl
    if ($null -ne $Config.analysis.tolerance) {
        return [pscustomobject]@{compared=$false;status='not_compared';reason='独立分析容差未传入本脚本的Minitab命令';program_value=$NativeMetrics.PT_pct}
    }
    if ($both) {
        if ($null -eq $NativeMetrics.PT_pct -or $null -eq $MinitabTolerance) {
            throw '双侧P/T比较缺少程序值或Minitab值'
        }
        $actual = [double]$NativeMetrics.PT_pct
        $expected = [double]$MinitabTolerance
        if ([double]::IsNaN($actual) -or [double]::IsInfinity($actual) -or
            [double]::IsNaN($expected) -or [double]::IsInfinity($expected) -or
            [Math]::Abs($actual-$expected) -gt 1e-11+1e-9*[Math]::Abs($expected)) {
            throw '双侧P/T与Minitab不一致'
        }
        return [pscustomobject]@{compared=$true;status='compared';reason='已比较Minitab双侧容差结果';program_value=$actual;minitab_value=$expected}
    }
    if ($any) {
        return [pscustomobject]@{compared=$false;status='not_compared';reason='单侧规格未传入本脚本的Minitab命令；不得记为单侧P/T对标通过';program_value=$NativeMetrics.PT_pct}
    }
    if ($null -ne $NativeMetrics.PT_pct) {
        throw '没有规格限或独立分析容差时P/T应不可计算'
    }
    return [pscustomobject]@{compared=$false;status='not_applicable';reason='无规格限或独立分析容差，P/T不可计算';program_value=$null}
}
