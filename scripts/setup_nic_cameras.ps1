#Requires -RunAsAdministrator
<#
.SYNOPSIS
    One-time setup for 6-camera GigE network on dedicated NIC ports.

.DESCRIPTION
    Each camera gets a dedicated 1 GbE port on its own subnet, eliminating the
    shared-switch bandwidth bottleneck (6 x ~52 MB/s > 125 MB/s).

    *** ROOM-SPECIFIC — filled in for this PC via VideoGrabber::enumerate_devices() ***
    The $CameraMap table below is this room's confirmed NIC-to-camera-IP-to-serial
    mapping, captured by discovering all 6 physically-connected cameras. If this
    script is copied to a different PC/room, re-derive it:
      1. Run 'Get-NetAdapter' on the new PC and note the real port names
         (e.g. "Ethernet 3") — they are specific to that PC's NIC hardware.
      2. Use Mosaic's "Discover cameras" button (Video settings tab) or
         Pylon IP Configurator's device list to read each camera's serial/IP.
      3. Replace $CameraMap below with the new room's values.
    Running this script with placeholder values still present will abort (see the
    preflight check) rather than silently apply a wrong/stale mapping.

    After running this script:
      1. Physically connect each camera to its assigned NIC port (see table).
      2. Open "Pylon IP Configurator" and assign each camera its static IP.
      3. In each camera's settings, set PacketSize to 8192 bytes (once jumbo
         frames are confirmed active post-reboot — see step 4 below).
#>

# ── Room-specific camera map ────────────────────────────────────────────────
# Nic:       Windows adapter name from `Get-NetAdapter` (e.g. "Ethernet 3")
# PcIp:      This PC's IP on that camera's dedicated subnet
# CameraIp:  Static IP to assign the camera via Pylon IP Configurator
# Serial:    Camera's serial number (physical label / Pylon device list)
# Label:     Human-readable camera identifier for this room (matches the
#            "Camera N" position shown in Mosaic's Video settings tab)
#
# Confirmed via VideoGrabber::enumerate_devices() with all 6 cameras connected.
# Ethernet 7's PC-side IP (192.168.8.2/24), jumbo frames, and receive buffers
# are now set to match the other 5 ports. The camera's own static IP
# (192.168.8.3) is still a manual Pylon IP Configurator step — it currently
# self-assigns a link-local address, which works fine for discovery/streaming
# but is inconsistent with the rest of the topology.
$CameraMap = @(
    [PSCustomObject]@{ Nic = "Ethernet 10"; PcIp = "192.168.3.2"; CameraIp = "192.168.3.3"; Serial = "24925616"; Label = "Camera 1" }
    [PSCustomObject]@{ Nic = "Ethernet 9";  PcIp = "192.168.7.2"; CameraIp = "192.168.7.3"; Serial = "24925618"; Label = "Camera 2" }
    [PSCustomObject]@{ Nic = "Ethernet 8";  PcIp = "192.168.6.2"; CameraIp = "192.168.6.3"; Serial = "24925620"; Label = "Camera 3" }
    [PSCustomObject]@{ Nic = "Ethernet 3";  PcIp = "192.168.4.2"; CameraIp = "192.168.4.3"; Serial = "24925615"; Label = "Camera 4" }
    [PSCustomObject]@{ Nic = "Ethernet 7";  PcIp = "192.168.8.2"; CameraIp = "192.168.8.3"; Serial = "24925621"; Label = "Camera 5 (camera-side static IP still pending — see note above)" }
    [PSCustomObject]@{ Nic = "Ethernet 6";  PcIp = "192.168.5.2"; CameraIp = "192.168.5.3"; Serial = "24893039"; Label = "Camera 6" }
)

Write-Host "`n=== MOSAIC camera NIC setup ===" -ForegroundColor Cyan

# ── Preflight: refuse to run against an un-edited template ─────────────────
$placeholderCount = ($CameraMap | Where-Object {
    $_.Nic -eq "REPLACE_ME" -or $_.PcIp -eq "REPLACE_ME" -or
    $_.CameraIp -eq "REPLACE_ME" -or $_.Serial -eq "REPLACE_ME"
}).Count
if ($placeholderCount -gt 0) {
    Write-Host "`nERROR: `$CameraMap still has $placeholderCount placeholder entr$(if ($placeholderCount -eq 1) {'y'} else {'ies'})." -ForegroundColor Red
    Write-Host "Edit the `$CameraMap table at the top of this script with this room's" -ForegroundColor Red
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
Write-Host "  4. Reboot to activate jumbo frames, then in Mosaic camera settings set"
Write-Host "     PacketSize = 8192 for each camera (the app itself still hardcodes 1500"
Write-Host "     bytes in video_grabber.cpp until that's made configurable — see the"
Write-Host "     room-11 bring-up plan for this known gap)."
Write-Host ""
