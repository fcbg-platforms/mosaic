# MOSAIC analysis environment setup — Windows (PowerShell)
# Usage: pwsh -File analysis\setup.ps1
param()
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$VenvDir   = Join-Path $ScriptDir '.venv'

Write-Host "==> Creating Python virtual environment at $VenvDir"
python -m venv $VenvDir

& "$VenvDir\Scripts\Activate.ps1"

Write-Host "==> Upgrading pip"
pip install --upgrade pip wheel

Write-Host "==> Installing analysis dependencies"
pip install -r (Join-Path $ScriptDir 'requirements.txt')

Write-Host ""
Write-Host "==> Setup complete.  To activate the environment:"
Write-Host "    analysis\.venv\Scripts\Activate.ps1"
Write-Host ""
Write-Host "==> To run pose estimation on a recorded session:"
Write-Host "    python analysis\run_pose.py --session C:\path\to\session"
Write-Host ""
Write-Host "==> YOLOv8 model weights (~6 MB) will be downloaded on first run."
