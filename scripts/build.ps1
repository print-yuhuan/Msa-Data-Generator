param(
    [string]$Generator = 'Visual Studio 18 2026',
    [string]$BuildDirectory = 'build',
    [switch]$FullTest
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$buildRoot = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    [IO.Path]::GetFullPath($BuildDirectory)
} else {
    [IO.Path]::GetFullPath((Join-Path $projectRoot $BuildDirectory))
}
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) { $cmakePath = $cmakeCommand.Source }
else {
    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vsPath = & $vswherePath -latest -products '*' -property installationPath
    $cmakePath = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
$options = @()
if ($FullTest) { $options = @('-DMSA_BUILD_GUI=ON', '-DMSA_BUILD_RENDER_TESTS=ON') }
& $cmakePath -S $projectRoot -B $buildRoot -G $Generator -A x64 @options
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $cmakePath --build $buildRoot --config Release --parallel 6
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($FullTest) {
    $ctestPath = Join-Path (Split-Path $cmakePath -Parent) 'ctest.exe'
    & $ctestPath --test-dir $buildRoot -C Release --output-on-failure --no-tests=error
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
