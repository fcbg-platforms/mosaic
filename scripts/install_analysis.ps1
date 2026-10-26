<#
.SYNOPSIS
    Installs MOSAIC for analysis only: no cameras, no lab hardware.

.DESCRIPTION
    For a PC that analyses recordings made in the room. It builds MOSAIC
    without the Basler camera, FFmpeg, NVENC and OpenCV parts (so neither
    Pylon nor vcpkg is needed), copies the Qt libraries next to mosaic.exe,
    and makes the analysis Python environment (analysis/.venv). MOSAIC then
    opens on the Analysis tab.

    Needs, installed first:
      - Visual Studio 2022, "Desktop development with C++"
      - CMake 3.25+ on the PATH (Visual Studio's own, from a Developer
        PowerShell, will do)
      - Qt 6.8+ (msvc2022_64) with Qt Multimedia, Charts, Serial Port and
        Quick 3D; found as scripts/configure.ps1 finds it (MOSAIC_QT_PREFIX,
        else C:\Qt)
      - uv (https://docs.astral.sh/uv/) and git (uv fetches one package
        from GitHub)

    Running it again rebuilds and updates the Python environment. It refuses
    to touch a build/Release made with cameras (a recording PC).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\install_analysis.ps1
#>

$root     = Split-Path $PSScriptRoot -Parent
$buildDir = Join-Path $root "build/Release"

# Without vcpkg nothing optional is fetched (vcpkg.json would build FFmpeg
# and OpenCV, which an analysis PC does not need). Put back on the way out,
# for when this runs in the caller's own PowerShell.
$savedVcpkgRoot = $env:VCPKG_ROOT

function Fail([string]$Message) {
    Write-Host $Message -ForegroundColor Red
    $env:VCPKG_ROOT = $savedVcpkgRoot
    exit 1
}

foreach ($tool in @("cmake", "uv", "git")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Fail "$tool not found on the PATH: install it first (see the top of this script)."
    }
}

# A recording PC's build has the cameras on: reconfiguring it would take
# them out. A build once made with vcpkg keeps using it, whatever the
# environment says.
$cache = Join-Path $buildDir "CMakeCache.txt"
if ((Test-Path $cache) -and (Select-String -Path $cache -Pattern "^MOSAIC_ENABLE_CAMERAS:BOOL=ON" -Quiet)) {
    Fail "$buildDir is a build with cameras (a recording PC): left as it is."
}
if ((Test-Path $cache) -and (Select-String -Path $cache -Pattern "^CMAKE_TOOLCHAIN_FILE:.*vcpkg" -Quiet)) {
    Fail "$buildDir was made with vcpkg: delete that folder, then run this again."
}
Remove-Item Env:VCPKG_ROOT -ErrorAction SilentlyContinue

Write-Host "1/4  Configuring MOSAIC without cameras" -ForegroundColor Cyan
& (Join-Path $PSScriptRoot "configure.ps1") -BuildType Release
if ($LASTEXITCODE -ne 0) {
    Fail ("Configuring failed. A missing Qt module (Quick3D, ShaderTools, ...) is named " +
          "above: add it with the Qt Maintenance Tool, then run this again.")
}

Write-Host "2/4  Building" -ForegroundColor Cyan
& cmake --build $buildDir --config Release --parallel --target mosaic
if ($LASTEXITCODE -ne 0) { Fail "The build failed (see above)." }

Write-Host "3/4  Copying the Qt libraries next to mosaic.exe" -ForegroundColor Cyan
$exe = Get-ChildItem (Join-Path $buildDir "bin") -Recurse -Filter mosaic.exe | Select-Object -First 1
if (-not $exe) { Fail "mosaic.exe not found under $buildDir\bin." }
$qtPrefix = (Select-String -Path $cache -Pattern "^CMAKE_PREFIX_PATH:\w+=(.+)$").Matches[0].Groups[1].Value
& (Join-Path $qtPrefix "bin/windeployqt.exe") --release --qmldir (Join-Path $root "src/qml") $exe.FullName
if ($LASTEXITCODE -ne 0) { Fail "windeployqt failed (see above)." }

Write-Host "4/4  Making the analysis Python environment (a few GB, the first time)" -ForegroundColor Cyan
Push-Location (Join-Path $root "analysis")
try {
    & uv sync
    if ($LASTEXITCODE -ne 0) { Fail "uv sync failed (see above)." }
} finally {
    Pop-Location
}

$env:VCPKG_ROOT = $savedVcpkgRoot

Write-Host ""
Write-Host "Done. Start MOSAIC with:" -ForegroundColor Green
Write-Host "  $($exe.FullName)"
Write-Host "It opens on the Analysis tab. Copy the recorded sessions into"
Write-Host "  $(Join-Path $root 'recordings')\<your profile>"
