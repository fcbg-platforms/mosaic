# This room's camera network: which NIC port each camera is plugged into, the
# PC's and the camera's static IPs on that port's subnet, and the camera's
# serial. Read by setup_nic_cameras.ps1 (applies the NIC settings) and
# doctor.ps1 (checks them).
#
# *** ROOM-SPECIFIC: confirmed for room 11 via VideoGrabber::enumerate_devices()
# with all 6 cameras connected. *** On another PC, re-derive it:
#   1. Run 'Get-NetAdapter' and note the real port names (e.g. "Ethernet 3").
#   2. Read each camera's serial and IP with MOSAIC's "Discover cameras" button
#      (Video settings tab) or Pylon IP Configurator's device list.
#   3. Replace the table below. setup_nic_cameras.ps1 refuses to run while any
#      value is still "REPLACE_ME".
#
# Nic:      Windows adapter name from Get-NetAdapter
# PcIp:     this PC's IP on that camera's dedicated subnet
# CameraIp: static IP to give the camera in Pylon IP Configurator
# Serial:   the camera's serial number (physical label / Pylon device list)
# Label:    the "Camera N" position shown in MOSAIC's Video settings tab
@{
    Cameras = @(
        @{ Nic = "Ethernet 10"; PcIp = "192.168.3.2"; CameraIp = "192.168.3.3"; Serial = "24925616"; Label = "Camera 1" }
        @{ Nic = "Ethernet 9";  PcIp = "192.168.7.2"; CameraIp = "192.168.7.3"; Serial = "24925618"; Label = "Camera 2" }
        @{ Nic = "Ethernet 8";  PcIp = "192.168.6.2"; CameraIp = "192.168.6.3"; Serial = "24925620"; Label = "Camera 3" }
        @{ Nic = "Ethernet 3";  PcIp = "192.168.4.2"; CameraIp = "192.168.4.3"; Serial = "24925615"; Label = "Camera 4" }
        @{ Nic = "Ethernet 7";  PcIp = "192.168.8.2"; CameraIp = "192.168.8.3"; Serial = "24925621"; Label = "Camera 5" }
        @{ Nic = "Ethernet 6";  PcIp = "192.168.5.2"; CameraIp = "192.168.5.3"; Serial = "24893039"; Label = "Camera 6" }
    )
}
