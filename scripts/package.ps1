param([string]$BuildDirectory='build')
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'release-common.ps1')
$info=Get-ReleaseInfo $projectRoot $BuildDirectory
$packageRoot=$info.PackageRoot
foreach ($destination in @($packageRoot, "$packageRoot.zip", "$packageRoot.zip.sha256")) {
    if (Test-Path -LiteralPath $destination) {
        throw "Package artifact already exists; preserve or rename it before rebuilding: $destination"
    }
}
$distRoot = [IO.Path]::GetDirectoryName($packageRoot)
$stageRoot = [IO.Path]::GetFullPath((Join-Path $distRoot ('.stage-' + $info.Version + '-' + [guid]::NewGuid().ToString('N'))))
if ([IO.Path]::GetDirectoryName($stageRoot) -ne $distRoot) { throw 'Invalid package staging directory' }
$stage = Join-Path $stageRoot ([IO.Path]::GetFileName($packageRoot))
$runRoot=Join-Path $info.BuildRoot ('release-verification\'+[guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Force -Path $stage,(Join-Path $stage 'docs\checks'),(Join-Path $stage 'tests\fixtures') | Out-Null
    foreach($binary in 'msa_generator.exe','msa_cli.exe','msa_tests.exe'){Copy-Item -LiteralPath (Join-Path $info.BuildRoot "Release\$binary") -Destination $stage}
    foreach($directory in 'licenses','examples'){Copy-Item -LiteralPath (Join-Path $projectRoot $directory) -Destination $stage -Recurse}
    Copy-ProjectLicense $stage $projectRoot
    foreach($document in 'README.md','THIRD_PARTY_NOTICES.md','dependencies.lock.json'){Copy-Item -LiteralPath (Join-Path $projectRoot $document) -Destination $stage}
    Copy-Item -LiteralPath (Join-Path $info.BuildRoot 'release-version.json') -Destination $stage
    Get-ChildItem -LiteralPath (Join-Path $projectRoot 'docs') -Filter *.md | Copy-Item -Destination (Join-Path $stage 'docs')
    foreach($asset in 'app-icon-preview.png','ui-preview.png','chart-origin-comparison.png','chart-styles.png','six-row-import.png'){
        Copy-Item -LiteralPath (Join-Path $projectRoot "docs\$asset") -Destination (Join-Path $stage 'docs')
    }
    Copy-Item -LiteralPath (Join-Path $projectRoot 'tests\fixtures\minitab_baseline.json') -Destination (Join-Path $stage 'tests\fixtures')
    & (Join-Path $PSScriptRoot 'verify-release.ps1') -BuildDirectory $info.BuildRoot -PackageDirectory $stage -ReportDirectory $runRoot
    $checks=Join-Path $stage 'docs\checks'
    Copy-Item -LiteralPath (Join-Path $runRoot 'native-artifact-checks.json'),(Join-Path $runRoot 'native\native-regression.json'),(Join-Path $runRoot 'cli\cli-regression.json'),(Join-Path $info.BuildRoot 'release-test-output\release-regression.json') -Destination $checks
    Publish-RuntimePackage $stage $packageRoot
} finally {
    if (Test-Path -LiteralPath $stageRoot) {
        Remove-Item -LiteralPath $stageRoot -Recurse -Force -ErrorAction Continue
    }
}
