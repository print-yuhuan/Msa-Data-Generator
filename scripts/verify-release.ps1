param([string]$BuildDirectory='build',[string]$PackageDirectory='',[string]$ReportDirectory='',[string]$ExpectedUserConfigSha256='')
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'release-common.ps1')
$info=Get-ReleaseInfo $projectRoot $BuildDirectory
$packageRoot=if($PackageDirectory){[IO.Path]::GetFullPath($PackageDirectory)}else{$info.PackageRoot}
$runRoot=if($ReportDirectory){$ReportDirectory}else{Join-Path $info.BuildRoot ('release-verification\'+[guid]::NewGuid().ToString('N'))}
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$checks=[System.Collections.Generic.List[object]]::new()
function Check([bool]$Passed,[string]$Name){if(-not $Passed){throw "FAIL: $Name"};$checks.Add([pscustomobject]@{name=$Name;passed=$true})}
Assert-ProjectLicense $packageRoot $projectRoot
Check $true 'project LICENSE and NOTICE packaged without modification'
$lock=Get-Content -LiteralPath (Join-Path $projectRoot 'dependencies.lock.json') -Raw -Encoding UTF8 | ConvertFrom-Json
Check ((Get-FileHash -LiteralPath (Join-Path $projectRoot 'resources\fonts\NotoSansCJKsc-Regular.otf')).Hash -eq $lock.font.sha256) 'source font matches locked SHA256'
$embeddedFontHash = Assert-EmbeddedFont (Join-Path $packageRoot 'msa_generator.exe') $lock.font.sha256
Check $true 'GUI embeds the complete locked Chinese font'
Check (-not (Test-Path -LiteralPath (Join-Path $packageRoot 'resources'))) 'runtime package needs no external resources directory'
Check ((Get-FileHash -LiteralPath (Join-Path $projectRoot 'third_party\nlohmann\json.hpp')).Hash -eq $lock.json.sha256) 'JSON library matches locked SHA256'
$hashes=@{}
foreach($binary in 'msa_generator.exe','msa_cli.exe','msa_tests.exe'){
    $actual=(Get-FileHash -LiteralPath (Join-Path $packageRoot $binary)).Hash
    Check ($actual -eq (Get-FileHash -LiteralPath (Join-Path $info.BuildRoot "Release\$binary")).Hash) "$binary matches current Release build"
    $hashes[$binary]=$actual
}
Check (@(Get-ChildItem -LiteralPath $packageRoot -File -Recurse | Where-Object { $_.Extension -in '.py','.pyw','.dll' }).Count -eq 0) 'runtime package has no Python or extra runtime DLLs'
foreach($license in 'Noto-OFL.txt','FreeType-FTL.txt','EUI-NEO-Apache-2.0.txt'){Check (Test-Path -LiteralPath (Join-Path $packageRoot "licenses\$license")) "$license packaged"}
$fixture=Join-Path $packageRoot 'tests\fixtures\minitab_baseline.json'
$baselineHash=(Get-FileHash -LiteralPath $fixture).Hash
Check ($baselineHash -eq '08815D5CB98D15D50F4741893875FFAE560B1D88AF53D05F0B9A124D4C2B8E2D') 'packaged independent baseline unchanged'
Check ($baselineHash -eq (Get-FileHash -LiteralPath (Join-Path $projectRoot 'tests\fixtures\minitab_baseline.json')).Hash) 'source and package baseline identical'
$bound=@{'msa_cli.exe'=$hashes['msa_cli.exe'];'msa_tests.exe'=$hashes['msa_tests.exe'];'minitab_baseline.json'=$baselineHash}
$original=Get-Content -LiteralPath $fixture -Raw -Encoding UTF8 | ConvertFrom-Json
# Fresh tests run from the package, never from historical pass flags.
& (Join-Path $packageRoot 'msa_tests.exe') $fixture (Join-Path $runRoot 'native')
if($LASTEXITCODE -ne 0){throw 'Packaged native regression failed'}
$reg=Get-Content -LiteralPath (Join-Path $runRoot 'native\native-regression.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$nativeReportPath=Join-Path $runRoot 'native\native-regression.json'
$nativeReportHash=(Get-FileHash -LiteralPath $nativeReportPath).Hash
$binding=[pscustomobject]@{passed=$true;version=$info.NativeVersion;artifacts=[pscustomobject]$bound;native_report_sha256=$nativeReportHash}
Assert-EvidenceBinding $binding $bound $nativeReportPath $info.NativeVersion
Check $true 'fresh native evidence bound to current binaries and baseline'
Check ($reg.version -eq $info.NativeVersion -and $reg.failed -eq 0 -and $reg.passed -eq $reg.tests.Count -and $reg.passed -gt 0) 'fresh packaged native groups passed'
Check ($reg.baseline.Count -eq 192 -and $reg.baseline_designs -eq 24 -and @($reg.baseline | Where-Object {-not $_.passed}).Count -eq 0) 'fresh packaged 192 baselines and 24 designs passed'
Check (@(Compare-Object @($original.cases.id | Sort-Object) @($reg.baseline.id | Sort-Object)).Count -eq 0) 'fresh baseline case identities match'
& (Join-Path $projectRoot 'tests\test_cli.ps1') -Cli (Join-Path $packageRoot 'msa_cli.exe') -OutputDirectory (Join-Path $runRoot 'cli') -ExpectedVersion $info.Version
$cli=Get-Content -LiteralPath (Join-Path $runRoot 'cli\cli-regression.json') -Raw -Encoding UTF8 | ConvertFrom-Json
Check ($cli.passed -and $cli.version -eq $info.NativeVersion) 'fresh packaged CLI regressions passed'
& (Join-Path $projectRoot 'tests\test_release.ps1') -BuildDirectory $info.BuildRoot
$release=Get-Content -LiteralPath (Join-Path $info.BuildRoot 'release-test-output\release-regression.json') -Raw -Encoding UTF8 | ConvertFrom-Json
Check ($release.passed -and $release.version -eq $info.NativeVersion) 'stale evidence rejection regressions passed'
foreach($binary in $hashes.Keys){Check ((Get-FileHash -LiteralPath (Join-Path $packageRoot $binary)).Hash -eq $hashes[$binary]) "$binary unchanged during tests"}
if($ExpectedUserConfigSha256){Check ((Get-FileHash -LiteralPath (Join-Path $env:USERPROFILE 'MSA数据生成器\msa_generator.json')).Hash -eq $ExpectedUserConfigSha256) 'user configuration unchanged'}
$report=[ordered]@{passed=$true;version=$info.NativeVersion;date_utc=[DateTime]::UtcNow.ToString('o');executable_sha256=$hashes;embedded_font_sha256=$embeddedFontHash;baseline_sha256=$baselineHash;native_report_sha256=$nativeReportHash;native_groups=$reg.passed;baseline_cases=$reg.baseline.Count;baseline_designs=$reg.baseline_designs;cli_checks=$cli.checks.Count;release_checks=$release.checks.Count;checks=$checks}
$report | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $runRoot 'native-artifact-checks.json') -Encoding UTF8
Write-Output "$($checks.Count) release checks PASS; $($reg.passed) native groups / 192 baselines / 24 designs"
