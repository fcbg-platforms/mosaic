# Quick configure script for Windows (PowerShell)
# Usage: .\scripts\configure.ps1 [-BuildType Release|Debug]

param(
    [ValidateSet("Debug","Release","RelWithDebInfo")]
    [string]$BuildType = "Release"
)

$root = Split-Path $PSScriptRoot -Parent
$buildDir = Join-Path $root "build/$BuildType"

cmake -S $root -B $buildDir `
    -DCMAKE_BUILD_TYPE=$BuildType `
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `
    -DMOSAIC_BUILD_TESTS=ON

Write-Host "`nConfigured. Build with:"
Write-Host "  cmake --build $buildDir --parallel"
