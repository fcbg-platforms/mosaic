#Requires -RunAsAdministrator
<#
.SYNOPSIS
    One-time setup for 6-camera GigE network on dedicated NIC ports.

.DESCRIPTION
    Each camera gets a dedicated 1 GbE port on its own subnet, eliminating the
    shared-switch bandwidth bottleneck (6 x ~52 MB/s > 125 MB/s).

    That ~52 MB/s assumes 1920x1080 at one byte per pixel (Bayer) and 25 fps.
    A two-byte format (YUV422) doubles it and a three-byte one (BGR8) triples
    it, past what one gigabit link carries. Which format a camera really sends
    is logged when MOSAIC opens it ("[Camera N] Pixel format ..."), with its
    share of the link; the pixel format in the settings is only a request.

    The room's NIC-to-camera map (port names, IPs, serials) is in
    room_cameras.psd1 next to this script, shared with doctor.ps1. Edit that
    file, not this one, when the room or PC changes; this script refuses to
    run while it still holds "REPLACE_ME" values. Run doctor.ps1 afterwards to
    check the result.

    After running this script:
      1. Physically connect each camera to its assigned NIC port (see table).
      2. Open "Pylon IP Configurator" and assign each camera its static IP.
      3. Do NOT raise PacketSize. This step used to say 8192 bytes; MOSAIC
         sets GevSCPSPacketSize to 1500 at every open, because 8192 was tested
         on this hardware and was a severe regression (packet errors exceeded
         received packets on every camera). Jumbo frames stay enabled on the
         NICs below, but the camera stream deliberately does not use them.
         See the GevSCPSPacketSize comment in src/video/video_grabber.cpp.
#>

# ── Room-specific camera map (see room_cameras.psd1) ──────────────────────
$CameraMap = (Import-PowerShellDataFile (Join-Path $PSScriptRoot "room_cameras.psd1")).Cameras |
    ForEach-Object { [PSCustomObject]$_ }

Write-Host "`n=== MOSAIC camera NIC setup ===" -ForegroundColor Cyan

# ── Preflight: refuse to run against an un-edited template ─────────────────
$placeholderCount = ($CameraMap | Where-Object {
    $_.Nic -eq "REPLACE_ME" -or $_.PcIp -eq "REPLACE_ME" -or
    $_.CameraIp -eq "REPLACE_ME" -or $_.Serial -eq "REPLACE_ME"
}).Count
if ($placeholderCount -gt 0) {
    Write-Host "`nERROR: `$CameraMap still has $placeholderCount placeholder entr$(if ($placeholderCount -eq 1) {'y'} else {'ies'})." -ForegroundColor Red
    Write-Host "Edit room_cameras.psd1 (next to this script) with this room's" -ForegroundColor Red
    Write-Host "real NIC names, IPs, and camera serials before running it. See the" -ForegroundColor Red
    Write-Host "header comment for how to find those values (Get-NetAdapter + camera labels)." -ForegroundColor Red
    exit 1
}

$cameraPorts = $CameraMap.Nic

# ── Step 1: Jumbo frames on all 6 camera ports ─────────────────────────────
Write-Host "`n[1/2] Setting jumbo frames (9014 bytes) ..." -ForegroundColor Yellow
foreach ($nic in $cameraPorts) {
    $success = $false

    # Try display-name form first (newer Intel drivers)
    try {
        Set-NetAdapterAdvancedProperty -Name $nic -DisplayName "Jumbo Packet" -DisplayValue "9014 Bytes" -ErrorAction Stop
        $success = $true
    } catch {}

    # Fallback: registry keyword form
    if (-not $success) {
        try {
            Set-NetAdapterAdvancedProperty -Name $nic -RegistryKeyword "*JumboPacket" -RegistryValue 9014 -ErrorAction Stop
            $success = $true
        } catch {}
    }

    $ip = (Get-NetIPAddress -InterfaceAlias $nic -AddressFamily IPv4 -ErrorAction SilentlyContinue).IPAddress
    if ($success) {
        Write-Host "  OK   $nic  ($ip)  -> jumbo=9014" -ForegroundColor Green
    } else {
        Write-Host "  WARN $nic  ($ip)  -> jumbo not set (check driver)" -ForegroundColor DarkYellow
    }
}

# ── Step 2: Receive buffers — increase for burst GigE traffic ─────────────
Write-Host "`n[2/2] Setting receive buffers ..." -ForegroundColor Yellow
foreach ($nic in $cameraPorts) {
    try {
        Set-NetAdapterAdvancedProperty -Name $nic -DisplayName "Receive Buffers" -DisplayValue "2048" -ErrorAction Stop
        Write-Host "  OK   $nic  -> recv_buf=2048" -ForegroundColor Green
    } catch {
        Write-Host "  SKIP $nic  -> $($_.Exception.Message)" -ForegroundColor DarkGray
    }
}

# ── Summary ────────────────────────────────────────────────────────────────
Write-Host "`n=== Done ===`n" -ForegroundColor Cyan
Write-Host "Camera IP assignment table:" -ForegroundColor White
Write-Host "  NIC          PC IP          Camera IP to set in Pylon"
foreach ($c in $CameraMap) {
    Write-Host "  $($c.Nic.PadRight(12)) $($c.PcIp.PadRight(14)) $($c.CameraIp)  ($($c.Label) - serial $($c.Serial))"
}
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "  1. Plug each camera into its assigned NIC port above"
Write-Host "  2. Open: C:\Program Files\Basler\pylon 7\Tools\PylonIPConfigurator.exe"
Write-Host "  3. For each camera: select it -> Set Static IP -> enter the IP from table above"
Write-Host "  4. Reboot to activate the NIC settings (MOSAIC keeps the camera packet"
Write-Host "     size at 1500 bytes on purpose; see the note at the top of this script)"
Write-Host "  5. Check everything with: .\scripts\doctor.ps1"
Write-Host ""
