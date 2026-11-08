param([Parameter(Mandatory)][string]$BuildDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'scripts\release-common.ps1')
$info=Get-ReleaseInfo $root $BuildDirectory
$runRoot=Join-Path $info.BuildRoot ('release-test-output\'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
$nativeReport=Join-Path $runRoot 'native-regression.json'
[IO.File]::WriteAllText($nativeReport,'{}')
$artifacts=@{'msa_cli.exe'='aaa';'msa_tests.exe'='bbb';'minitab_baseline.json'='ccc'}
$report=[pscustomobject]@{passed=$true;version=$info.NativeVersion;artifacts=[pscustomobject]$artifacts.Clone();native_report_sha256=(Get-FileHash -LiteralPath $nativeReport).Hash}
$checks=[System.Collections.Generic.List[object]]::new()
function Check([bool]$ok,[string]$name){if(-not $ok){throw "FAIL: $name"};$checks.Add([pscustomobject]@{name=$name;passed=$true})}
function Must-Reject([scriptblock]$Action,[string]$Name){$rejected=$false;try{& $Action}catch{$rejected=$true};Check $rejected $Name}
$lock=Get-Content -LiteralPath (Join-Path $root 'dependencies.lock.json') -Raw -Encoding UTF8 | ConvertFrom-Json
Must-Reject {Assert-EmbeddedFont (Join-Path $info.BuildRoot 'Release/msa_cli.exe') $lock.font.sha256} 'executable without embedded Chinese font rejected'
$guiPath=Join-Path $info.BuildRoot 'Release/msa_generator.exe'
if(Test-Path -LiteralPath $guiPath) {
    Check ((Assert-EmbeddedFont $guiPath $lock.font.sha256) -eq $lock.font.sha256) 'complete embedded font matches locked source'
    Must-Reject {Assert-EmbeddedFont $guiPath ('0'*64)} 'incorrect embedded font hash rejected'
}
$legalRoot=Join-Path $runRoot 'legal-package'
New-Item -ItemType Directory -Path $legalRoot | Out-Null
Copy-ProjectLicense $legalRoot $root
Assert-ProjectLicense $legalRoot $root
Check $true 'official package includes unchanged project license and informational notice'
$noticePath=Join-Path $legalRoot 'NOTICE'
Move-Item -LiteralPath $noticePath -Destination (Join-Path $legalRoot 'NOTICE.backup')
Must-Reject {Assert-ProjectLicense $legalRoot $root} 'official package missing informational notice rejected'
Move-Item -LiteralPath (Join-Path $legalRoot 'NOTICE.backup') -Destination $noticePath
[IO.File]::WriteAllText((Join-Path $legalRoot 'LICENSE'),'altered terms')
Must-Reject {Assert-ProjectLicense $legalRoot $root} 'altered project license rejected'
Copy-ProjectLicense $legalRoot $root
$dedicatedLicense=Join-Path $legalRoot 'licenses/MSA-Project-LICENSE.txt'
Move-Item -LiteralPath $dedicatedLicense -Destination "$dedicatedLicense.backup"
Must-Reject {Assert-ProjectLicense $legalRoot $root} 'missing dedicated project license rejected'
Move-Item -LiteralPath "$dedicatedLicense.backup" -Destination $dedicatedLicense
[IO.File]::WriteAllText($dedicatedLicense,'summary only')
Must-Reject {Assert-ProjectLicense $legalRoot $root} 'summary cannot replace full dedicated project license'
Assert-EvidenceBinding $report $artifacts $nativeReport $info.NativeVersion
Check $true 'matching evidence accepted'
$report.artifacts.'msa_cli.exe'='stale'
Must-Reject {Assert-EvidenceBinding $report $artifacts $nativeReport $info.NativeVersion} 'historical CLI hash rejected'
$report.artifacts.'msa_cli.exe'='aaa'
$report.artifacts.'msa_tests.exe'='stale'
Must-Reject {Assert-EvidenceBinding $report $artifacts $nativeReport $info.NativeVersion} 'historical test binary hash rejected'
$report.artifacts.'msa_tests.exe'='bbb'
$report.artifacts.'minitab_baseline.json'='stale'
Must-Reject {Assert-EvidenceBinding $report $artifacts $nativeReport $info.NativeVersion} 'changed baseline rejected'
$report.artifacts.'minitab_baseline.json'='ccc'
[IO.File]::WriteAllText($nativeReport,'{"changed":true}')
Must-Reject {Assert-EvidenceBinding $report $artifacts $nativeReport $info.NativeVersion} 'changed native evidence rejected'
$report.native_report_sha256=(Get-FileHash -LiteralPath $nativeReport).Hash
$report.version='old-native'
Must-Reject {Assert-EvidenceBinding $report $artifacts $nativeReport $info.NativeVersion} 'historical version rejected'
$report.version=$info.NativeVersion
$report.passed=$false
Must-Reject {Assert-EvidenceBinding $report $artifacts $nativeReport $info.NativeVersion} 'failed evidence rejected'
Check ($info.PackageRoot.Contains("v$($info.Version)_Windows_x64") -and $info.SourceZip.Contains("v$($info.Version)_Source.zip")) 'package names use generated version'
$manifest=[xml](Get-Content -LiteralPath (Join-Path $info.BuildRoot 'generated\app.manifest') -Raw -Encoding UTF8)
Check ($manifest.assembly.assemblyIdentity.version -eq "$($info.Version).0") 'embedded manifest uses generated version'
[ordered]@{passed=$true;version=$info.NativeVersion;checks=$checks} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $info.BuildRoot 'release-test-output\release-regression.json') -Encoding UTF8
Write-Output "$($checks.Count) release evidence regression checks PASS"
