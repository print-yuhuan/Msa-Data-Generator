param([string]$Generator = 'Visual Studio 18 2026')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) { $cmakePath = $cmakeCommand.Source }
else {
    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vsPath = & $vswherePath -latest -products '*' -property installationPath
    $cmakePath = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
& $cmakePath -S $projectRoot -B (Join-Path $projectRoot 'build') -G $Generator -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& $cmakePath --build (Join-Path $projectRoot 'build') --config Release --parallel 6
if ($LASTEXITCODE -ne 0) { throw 'CMake build failed' }
