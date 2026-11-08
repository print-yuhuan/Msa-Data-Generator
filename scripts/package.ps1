param([string]$BuildDirectory='build')
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'release-common.ps1')
$info=Get-ReleaseInfo $projectRoot $BuildDirectory
$packageRoot=$info.PackageRoot
if(Test-Path -LiteralPath $packageRoot){throw "Package already exists; preserve or rename it before rebuilding: $packageRoot"}
$stage=Join-Path $projectRoot ('dist\.stage-'+$info.Version+'-'+[guid]::NewGuid().ToString('N'))
$runRoot=Join-Path $info.BuildRoot ('release-verification\'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $stage,(Join-Path $stage 'docs\checks'),(Join-Path $stage 'tests\fixtures') | Out-Null
foreach($binary in 'msa_generator.exe','msa_cli.exe','msa_tests.exe'){Copy-Item -LiteralPath (Join-Path $info.BuildRoot "Release\$binary") -Destination $stage}
foreach($directory in 'licenses','examples'){Copy-Item -LiteralPath (Join-Path $projectRoot $directory) -Destination $stage -Recurse}
Copy-ProjectLicense $stage $projectRoot
foreach($document in 'README.md','THIRD_PARTY_NOTICES.md','dependencies.lock.json'){Copy-Item -LiteralPath (Join-Path $projectRoot $document) -Destination $stage}
Copy-Item -LiteralPath (Join-Path $info.BuildRoot 'release-version.json') -Destination $stage
Get-ChildItem -LiteralPath (Join-Path $projectRoot 'docs') -Filter *.md | Copy-Item -Destination (Join-Path $stage 'docs')
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\app-icon-preview.png') -Destination (Join-Path $stage 'docs')
foreach($directory in Get-ChildItem -LiteralPath (Join-Path $projectRoot 'docs') -Directory -Filter 'screenshots*'){Copy-Item -LiteralPath $directory.FullName -Destination (Join-Path $stage 'docs') -Recurse}
Copy-Item -LiteralPath (Join-Path $projectRoot 'tests\fixtures\minitab_baseline.json') -Destination (Join-Path $stage 'tests\fixtures')
& (Join-Path $PSScriptRoot 'verify-release.ps1') -BuildDirectory $info.BuildRoot -PackageDirectory $stage -ReportDirectory $runRoot
$checks=Join-Path $stage 'docs\checks'
Copy-Item -LiteralPath (Join-Path $runRoot 'native-artifact-checks.json'),(Join-Path $runRoot 'native\native-regression.json'),(Join-Path $runRoot 'cli\cli-regression.json'),(Join-Path $info.BuildRoot 'release-test-output\release-regression.json') -Destination $checks
Move-Item -LiteralPath $stage -Destination $packageRoot
$zipPath="$packageRoot.zip"
Compress-Archive -LiteralPath $packageRoot -DestinationPath $zipPath -Force
$hash=Get-FileHash -LiteralPath $zipPath -Algorithm SHA256
"$($hash.Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($zipPath))" | Set-Content -LiteralPath "$zipPath.sha256" -Encoding ASCII
Get-Item -LiteralPath $zipPath | Select-Object FullName,Length
