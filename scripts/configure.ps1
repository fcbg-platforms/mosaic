# Configure MOSAIC for Windows (PowerShell).
# Usage:
#   .\scripts\configure.ps1
#   .\scripts\configure.ps1 -BuildType Release -EnableCameras -EnableNvenc -EnableLsl
#
# Prerequisites:
#   • Visual Studio 2022 with "Desktop development with C++" workload
#   • Qt 6.4+ (msvc2022_64 kit) — set MOSAIC_QT_PREFIX or the default path is tried
#   • vcpkg at %VCPKG_ROOT% — run 'vcpkg install' in the repo root first
#   • Basler Pylon SDK 7.x at %PYLON_ROOT%   (only needed with -EnableCameras)
#   • CUDA Toolkit + driver                   (only needed with -EnableNvenc)
#   • InpOut32.dll next to mosaic.exe         (only needed with -EnableParallelPort)
#   • liblsl from vcpkg or %LSL_ROOT%         (only needed with -EnableLsl)

param(
    [ValidateSet("Debug","Release","RelWithDebInfo")]
    [string]$BuildType = "Release",

    [switch]$EnableCameras,
    [switch]$EnableNvenc,
    [switch]$EnableFfmpeg,
    [switch]$EnableParallelPort,
    [switch]$EnableLsl,
    [switch]$EnableOpenCV,
    [switch]$BuildTests
)

$root     = Split-Path $PSScriptRoot -Parent
$buildDir = Join-Path $root "build/$BuildType"

# ── Locate Qt 6 ────────────────────────────────────────────────────────────
function Find-Qt6 {
    # 1. Explicit env var
    if ($env:MOSAIC_QT_PREFIX -and (Test-Path "$env:MOSAIC_QT_PREFIX\lib\cmake\Qt6")) {
        return $env:MOSAIC_QT_PREFIX
    }
    # 2. Standard Qt installer paths
    foreach ($qtRoot in @("C:\Qt", "$env:USERPROFILE\Qt")) {
        if (-not (Test-Path $qtRoot)) { continue }
        $best = Get-ChildItem "$qtRoot\*\msvc2022_64\lib\cmake\Qt6\Qt6Config.cmake" `
                    -Recurse -ErrorAction SilentlyContinue |
                Sort-Object FullName -Descending |
                Select-Object -First 1
        if ($best) { return $best.Directory.Parent.Parent.Parent.FullName }
    }
    return $null
}

$qtPrefix = Find-Qt6
if (-not $qtPrefix) {
    Write-Error ("Qt 6 not found. Install via Qt Online Installer and set MOSAIC_QT_PREFIX " +
                  "to the msvc2022_64 directory (e.g. C:\Qt\6.8.1\msvc2022_64).")
    exit 1
}
Write-Host "Qt prefix: $qtPrefix"

# ── Build cmake args ───────────────────────────────────────────────────────
$cmakeArgs = @(
    "-S", $root,
    "-B", $buildDir,
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DCMAKE_PREFIX_PATH=$qtPrefix",
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    "-DMOSAIC_BUILD_TESTS=$(if ($BuildTests) { 'ON' } else { 'OFF' })",
    "-DMOSAIC_ENABLE_CAMERAS=$(if ($EnableCameras) { 'ON' } else { 'OFF' })",
    "-DMOSAIC_ENABLE_NVENC=$(if ($EnableNvenc) { 'ON' } else { 'OFF' })",
    "-DMOSAIC_ENABLE_FFMPEG=$(if ($EnableFfmpeg -or $EnableNvenc) { 'ON' } else { 'OFF' })",
    "-DMOSAIC_ENABLE_PARALLEL_PORT=$(if ($EnableParallelPort) { 'ON' } else { 'OFF' })",
    "-DMOSAIC_ENABLE_LSL=$(if ($EnableLsl) { 'ON' } else { 'OFF' })",
    "-DMOSAIC_ENABLE_OPENCV=$(if ($EnableOpenCV) { 'ON' } else { 'OFF' })"
)

if ($env:VCPKG_ROOT) {
    $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
}
if ($env:PYLON_ROOT) {
    $cmakeArgs += "-DPYLON_ROOT=$env:PYLON_ROOT"
}

# ── Configure ──────────────────────────────────────────────────────────────
Write-Host "Configuring MOSAIC ($BuildType)…"
& cmake @cmakeArgs

if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ""
Write-Host "Configured. Build with:"
Write-Host "  cmake --build $buildDir --parallel"
Write-Host ""
Write-Host "After building, deploy Qt DLLs for distribution:"
Write-Host "  windeployqt --qmldir $root\src\qml $buildDir\bin\mosaic.exe"
