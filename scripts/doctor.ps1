<#
.SYNOPSIS
    Checks that this PC is ready to record and analyse with MOSAIC.

.DESCRIPTION
    Read-only (no administrator rights needed, nothing is changed): it reports
    PASS / WARN / FAIL / INFO for

      System     Windows, CPU, memory, power plan
      GPU        model and driver, and whether it can hardware-encode every
                 camera at once (NVENC)
      Cameras    each camera's NIC port from room_cameras.psd1: link up and at
                 gigabit, this PC's IP, jumbo frames and receive buffers, the
                 camera answering; and the cameras in MOSAIC's settings
                 matching that map
      Streams    what each camera really sent when MOSAIC last opened it
                 (pixel format and share of the link, read from mosaic.log)
      Software   Basler Pylon, the MOSAIC build, both Python environments and
                 their key packages, the analysis model files
      Storage    the recordings folder: free space (in hours, at the configured
                 bitrate), drive type and a short write-speed test

    The exit code is 1 when any check fails, else 0.

.PARAMETER UserProfile
    Use this MOSAIC profile's settings (its folder name under
    %LOCALAPPDATA%\CSRU\MOSAIC\profiles). By default the most recently saved
    settings are used, whoever they belong to.

.PARAMETER SkipDiskTest
    Skip the write-speed test (it writes and deletes a 256 MB file in the
    recordings folder).

.PARAMETER Json
    Also write every result to this JSON file (for a support ticket).

.EXAMPLE
    .\scripts\doctor.ps1
.EXAMPLE
    .\scripts\doctor.ps1 -UserProfile csru -Json doctor_report.json
#>
param(
    [string]$UserProfile,
    [switch]$SkipDiskTest,
    [string]$Json
)

$ErrorActionPreference = "Continue"
$repo     = Split-Path $PSScriptRoot -Parent
$appData  = Join-Path $env:LOCALAPPDATA "CSRU\MOSAIC"
$results  = New-Object System.Collections.Generic.List[object]
$colours  = @{ PASS = "Green"; WARN = "Yellow"; FAIL = "Red"; INFO = "Gray" }

function Add-Result([string]$Section, [string]$Level, [string]$Check, [string]$Detail) {
    $results.Add([PSCustomObject]@{ Section = $Section; Level = $Level; Check = $Check; Detail = $Detail })
    Write-Host ("  {0,-4}  " -f $Level) -ForegroundColor $colours[$Level] -NoNewline
    Write-Host ("{0,-30} {1}" -f $Check, $Detail)
}

Add-Type -Namespace MosaicDoctor -Name Disk -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool GetDiskFreeSpaceEx(string directory, out ulong freeToCaller, out ulong total, out ulong totalFree);
'@

function Write-Section([string]$Title) {
    Write-Host "`n$Title" -ForegroundColor Cyan
}

function Get-Prop($Object, [string]$Name, $Default) {
    if ($null -ne $Object -and $Object.PSObject.Properties.Name -contains $Name) { return $Object.$Name }
    return $Default
}

Write-Host "`n=== MOSAIC doctor ===" -ForegroundColor Cyan
Write-Host "Repository: $repo"

# ---------------------------------------------------------------------------
# Settings
# ---------------------------------------------------------------------------
$settingsPath = $null
if ($UserProfile) {
    $settingsPath = Join-Path $appData "profiles\$UserProfile\settings.json"
} else {
    $candidates = @(Get-ChildItem (Join-Path $appData "profiles\*\settings.json") -ErrorAction SilentlyContinue)
    $candidates += @(Get-Item (Join-Path $appData "settings.json") -ErrorAction SilentlyContinue)
    $newest = $candidates | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($newest) { $settingsPath = $newest.FullName }
}
$settings = $null
if ($settingsPath -and (Test-Path $settingsPath)) {
    try {
        $settings = Get-Content $settingsPath -Raw -Encoding UTF8 | ConvertFrom-Json
        Write-Host "Settings:   $settingsPath"
        if (-not $UserProfile) { Write-Host "            (the most recently saved; pass -UserProfile <name> to choose)" }
    } catch {
        Write-Host "Settings:   could not read $settingsPath ($($_.Exception.Message))" -ForegroundColor Yellow
    }
} elseif ($UserProfile) {
    $known = @(Get-ChildItem (Join-Path $appData "profiles") -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name)
    Write-Host "Settings:   no profile '$UserProfile' (profiles: $($known -join ', '))" -ForegroundColor Yellow
} else {
    Write-Host "Settings:   none found (MOSAIC has not been run for this Windows user yet)" -ForegroundColor Yellow
}
$video  = Get-Prop $settings "video" $null
$record = Get-Prop $settings "record" $null
$audio  = Get-Prop $settings "audio" $null
$configuredCameras = @(Get-Prop $video "cameras" @())
$interview = Get-Prop $video "interview" $null
$interviewOn = [bool](Get-Prop $interview "enabled" $false)
$codec = [string](Get-Prop $video "codec" "")
# Interview mode records only its one camera.
$videoOn = [bool](Get-Prop $record "enable_video" $true)
$recordingCameras = if (-not $videoOn) { 0 } elseif ($interviewOn) { [math]::Min(1, $configuredCameras.Count) } else { $configuredCameras.Count }
$camerasText = if ($recordingCameras -eq 1) { "1 camera" } else { "$recordingCameras cameras" }
if ($interviewOn) { $camerasText += " (interview mode)" }

# ---------------------------------------------------------------------------
# System
# ---------------------------------------------------------------------------
Write-Section "System"
$os = Get-CimInstance Win32_OperatingSystem
Add-Result "System" "INFO" "Windows" "$($os.Caption) $($os.Version)"
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
Add-Result "System" "INFO" "CPU" ("{0} ({1} cores, {2} threads)" -f $cpu.Name.Trim(), $cpu.NumberOfCores, $cpu.NumberOfLogicalProcessors)
$ramGb = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
if ($ramGb -lt 16) {
    Add-Result "System" "WARN" "Memory" "$ramGb GB; 16 GB or more is recommended for six cameras and the analyses"
} else {
    Add-Result "System" "PASS" "Memory" "$ramGb GB"
}
$scheme = (powercfg /getactivescheme) -join " "
$planName = if ($scheme -match "\(([^)]+)\)") { $Matches[1] } else { $scheme }
# By GUID: the plan names are translated with Windows.
$fastPlans = "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c|e9a42b02-d5df-448d-aa00-03f14749eb61"
if ($scheme -match $fastPlans) {
    Add-Result "System" "PASS" "Power plan" $planName
} else {
    Add-Result "System" "WARN" "Power plan" "$planName; High performance is recommended while recording (Balanced lets the CPU and PCIe links save power, which can delay camera packets)"
}

# ---------------------------------------------------------------------------
# GPU
# ---------------------------------------------------------------------------
Write-Section "GPU"
$gpus = @(Get-CimInstance Win32_VideoController)
foreach ($g in $gpus) {
    Add-Result "GPU" "INFO" "Display adapter" "$($g.Name) (driver $($g.DriverVersion))"
}
$nvidia = $null
$smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
if ($smi) {
    $line = & nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>$null | Select-Object -First 1
    if ($line) {
        $parts = $line -split ",\s*"
        $nvidia = [PSCustomObject]@{ Name = $parts[0]; Driver = $parts[1] }
        Add-Result "GPU" "INFO" "NVIDIA GPU" "$($nvidia.Name), driver $($nvidia.Driver)"
    }
}
if (-not $videoOn) {
    Add-Result "GPU" "INFO" "Hardware encoding" "not used (video recording is off)"
} elseif ($codec -match "nvenc") {
    if (-not $nvidia) {
        Add-Result "GPU" "FAIL" "Hardware encoding" "the codec is $codec but no NVIDIA GPU was found; choose the CPU codec (libx264) in Settings, Video"
    } elseif ($nvidia.Name -match "GeForce|TITAN") {
        # Consumer cards cap how many NVENC sessions run at once; the cap
        # depends on the driver (see NVIDIA's "Video Encode and Decode GPU
        # Support Matrix"). Professional cards have no such cap.
        Add-Result "GPU" "WARN" "Hardware encoding" "$($nvidia.Name) is a consumer card, whose drivers limit how many streams it encodes at once; $camerasText record at once. Check NVIDIA's encode support matrix for this driver, or record a test session with every camera"
    } else {
        Add-Result "GPU" "PASS" "Hardware encoding" "$($nvidia.Name) is a professional card: no limit on concurrent encodes ($camerasText, $codec)"
    }
} elseif ($codec) {
    Add-Result "GPU" "INFO" "Hardware encoding" "not used (codec $codec)"
}

# ---------------------------------------------------------------------------
# Camera network
# ---------------------------------------------------------------------------
Write-Section "Camera network"
$mapPath = Join-Path $PSScriptRoot "room_cameras.psd1"
$map = @()
try {
    $map = @((Import-PowerShellDataFile $mapPath).Cameras)
} catch {
    Add-Result "Cameras" "FAIL" "Camera map" "could not read $mapPath ($($_.Exception.Message))"
}
foreach ($c in $map) {
    $label = $c.Label
    if (@($c.Nic, $c.PcIp, $c.CameraIp, $c.Serial) -contains "REPLACE_ME") {
        Add-Result "Cameras" "FAIL" $label "room_cameras.psd1 still holds placeholder values"
        continue
    }
    $nic = Get-NetAdapter -Name $c.Nic -ErrorAction SilentlyContinue
    if (-not $nic) {
        Add-Result "Cameras" "FAIL" "$label port" "no network adapter named '$($c.Nic)' (see Get-NetAdapter, then room_cameras.psd1)"
        continue
    }
    if ($nic.Status -ne "Up") {
        Add-Result "Cameras" "FAIL" "$label port" "$($c.Nic) is $($nic.Status): check the camera's cable and power"
        continue
    }
    $problems = @()
    if ($nic.ReceiveLinkSpeed -lt 1e9) { $problems += "link at $($nic.LinkSpeed), not 1 Gbps (cable or port fault)" }
    $ips = @(Get-NetIPAddress -InterfaceAlias $c.Nic -AddressFamily IPv4 -ErrorAction SilentlyContinue).IPAddress
    if ($ips -notcontains $c.PcIp) { $problems += "PC IP is $($ips -join ', '), expected $($c.PcIp)" }
    $adv = @(Get-NetAdapterAdvancedProperty -Name $c.Nic -RegistryKeyword "*JumboPacket", "*ReceiveBuffers" -ErrorAction SilentlyContinue)
    $jumbo = ($adv | Where-Object RegistryKeyword -eq "*JumboPacket").RegistryValue
    $rxbuf = ($adv | Where-Object RegistryKeyword -eq "*ReceiveBuffers").RegistryValue
    # Driver values are strings, and not every driver's are plain numbers.
    if ($jumbo -and "$($jumbo[0])" -match "^\d+$" -and [int]$jumbo[0] -lt 9000) { $problems += "jumbo frames $($jumbo[0]), expected 9014" }
    if ($rxbuf -and "$($rxbuf[0])" -match "^\d+$" -and [int]$rxbuf[0] -lt 2048) { $problems += "receive buffers $($rxbuf[0]), expected 2048" }
    $detail = "$($c.Nic), $($nic.LinkSpeed), PC $($c.PcIp)"
    if ($problems.Count -gt 0) {
        Add-Result "Cameras" "WARN" "$label port" ("$detail; " + ($problems -join "; ") + " (fix with setup_nic_cameras.ps1, as administrator)")
    } else {
        Add-Result "Cameras" "PASS" "$label port" $detail
    }
    if (Test-Connection $c.CameraIp -Count 1 -Quiet -ErrorAction SilentlyContinue) {
        Add-Result "Cameras" "PASS" "$label answers" "$($c.CameraIp) (serial $($c.Serial))"
    } else {
        Add-Result "Cameras" "WARN" "$label answers" "no reply at $($c.CameraIp): the camera may be off, or still on a link-local address (give it its static IP in Pylon IP Configurator)"
    }
}
if ($settings -and $map.Count -gt 0) {
    $mapSerials = @($map | ForEach-Object { [string]$_.Serial })
    $setSerials = @($configuredCameras | ForEach-Object { [string](Get-Prop $_ "serial" "") } | Where-Object { $_ })
    $unknown = @($setSerials | Where-Object { $mapSerials -notcontains $_ })
    $unused  = @($mapSerials | Where-Object { $setSerials -notcontains $_ })
    if ($unknown.Count -eq 0 -and $unused.Count -eq 0) {
        Add-Result "Cameras" "PASS" "Settings match the map" "$($setSerials.Count) cameras configured, all in room_cameras.psd1"
    } else {
        $parts = @()
        if ($unknown.Count) { $parts += "configured but not in the map: $($unknown -join ', ')" }
        if ($unused.Count)  { $parts += "in the map but not configured: $($unused -join ', ')" }
        Add-Result "Cameras" "WARN" "Settings match the map" ($parts -join "; ")
    }
}

# ---------------------------------------------------------------------------
# Last camera streams (what the cameras really sent, from mosaic.log)
# ---------------------------------------------------------------------------
Write-Section "Last camera streams"
# The log next to the settings in use: the same profile's last run (MOSAIC
# starts a fresh log at each launch).
$streamLines = @{}
$logUsed = $null
if ($settingsPath) {
    $log = Get-Item (Join-Path (Split-Path $settingsPath -Parent) "mosaic.log") -ErrorAction SilentlyContinue
    if ($log) {
        $pattern = '\[Camera (\d+)\] Pixel format (\S+)(.*?): (\d+)\D(\d+) @ ([\d.]+) fps = ([\d.]+) MB/s, (\d+)% of a gigabit link'
        foreach ($m in (Select-String -Path $log.FullName -Pattern $pattern -Encoding UTF8)) {
            $streamLines[$m.Matches[0].Groups[1].Value] = $m.Matches[0]
        }
        if ($streamLines.Count -gt 0) { $logUsed = $log }
    }
}
if (-not $logUsed) {
    Add-Result "Streams" "INFO" "Camera streams" "none logged yet: MOSAIC logs each camera's real pixel format and link use when it opens the camera"
} else {
    Write-Host "  (from $($logUsed.FullName), last written $($logUsed.LastWriteTime))"
    foreach ($key in ($streamLines.Keys | Sort-Object { [int]$_ })) {
        $g = $streamLines[$key].Groups
        $share = [int]$g[8].Value
        $detail = "{0} {1}x{2} @ {3} fps = {4} MB/s, {5}% of a gigabit link{6}" -f $g[2].Value, $g[4].Value, $g[5].Value, $g[6].Value, $g[7].Value, $share, $g[3].Value
        # The log numbers cameras from 0; the screen (and this report) from 1.
        $label = "Camera $([int]$key + 1)"
        # The same test as the app's own warning (k_gige_link_warn_utilisation),
        # on the rate rather than the rounded percentage.
        $mbps = [double]::Parse($g[7].Value, [Globalization.CultureInfo]::InvariantCulture)
        if ($mbps / 125.0 -gt 0.90) {
            Add-Result "Streams" "WARN" $label "$detail; it will lose frames: lower the frame rate or crop"
        } else {
            Add-Result "Streams" "PASS" $label $detail
        }
    }
}

# ---------------------------------------------------------------------------
# Software
# ---------------------------------------------------------------------------
Write-Section "Software"
$pylon = @()
if ($env:PYLON_ROOT -and (Test-Path $env:PYLON_ROOT)) { $pylon += Get-Item $env:PYLON_ROOT }
$pylon += @(Get-Item "C:\Program Files\Basler\pylon*" -ErrorAction SilentlyContinue)
if ($pylon.Count -gt 0) {
    Add-Result "Software" "PASS" "Basler Pylon" (($pylon | Select-Object -ExpandProperty FullName -Unique) -join ", ")
} else {
    Add-Result "Software" "FAIL" "Basler Pylon" "not found (install pylon 7, which MOSAIC needs to talk to the cameras)"
}

$exe = Get-ChildItem (Join-Path $repo "build\*\bin\*\mosaic.exe") -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($exe) {
    Add-Result "Software" "PASS" "MOSAIC build" "$($exe.FullName) (built $($exe.LastWriteTime))"
} else {
    Add-Result "Software" "WARN" "MOSAIC build" "no mosaic.exe under build\ (build with scripts\configure.ps1 and cmake --build)"
}

# Each environment: does the interpreter run, and do its key packages import?
$probe = @'
import importlib, json, sys
out = {"python": sys.version.split()[0]}
for name in sys.argv[1:]:
    try:
        mod = importlib.import_module(name)
        out[name] = getattr(mod, "__version__", "ok")
    except Exception as exc:
        out[name] = "MISSING: " + type(exc).__name__ + ": " + str(exc)[:120]
if "torch" in out and not str(out["torch"]).startswith("MISSING"):
    import torch
    out["cuda"] = torch.cuda.is_available()
print(json.dumps(out))
'@
$probeFile = Join-Path $env:TEMP "mosaic_doctor_probe.py"
Set-Content -Path $probeFile -Value $probe -Encoding ASCII
$envs = @(
    @{ Name = "Analysis Python"; Dir = "analysis"; Modules = @("numpy", "scipy", "cv2", "mediapipe", "ultralytics", "torch", "onnxruntime", "faster_whisper", "pyannote.audio") }
    @{ Name = "Real-time Python"; Dir = "python"; Modules = @("numpy", "cv2", "mediapipe") }
)
foreach ($e in $envs) {
    $py = Join-Path $repo "$($e.Dir)\.venv\Scripts\python.exe"
    if (-not (Test-Path $py)) {
        Add-Result "Software" "FAIL" $e.Name "no $($e.Dir)\.venv (create it with: cd $($e.Dir); uv sync)"
        continue
    }
    $raw = & $py $probeFile @($e.Modules) 2>$null | Select-Object -Last 1
    try { $info = $raw | ConvertFrom-Json } catch { $info = $null }
    if (-not $info) {
        Add-Result "Software" "FAIL" $e.Name "$py does not run"
        continue
    }
    $missing = @($e.Modules | Where-Object { ([string]$info.$_).StartsWith("MISSING") })
    $found = @($e.Modules | Where-Object { $missing -notcontains $_ } | ForEach-Object { "$_ $($info.$_)" })
    if ($missing.Count -gt 0) {
        Add-Result "Software" "FAIL" $e.Name ("Python $($info.python); cannot import " + ($missing -join ", ") + " (run uv sync in $($e.Dir))")
    } else {
        Add-Result "Software" "PASS" $e.Name ("Python $($info.python); " + ($found -join ", "))
    }
    if ($info.PSObject.Properties.Name -contains "cuda") {
        if ($info.cuda) {
            Add-Result "Software" "PASS" "$($e.Name) GPU" "PyTorch sees the GPU (pose and depth analyses run on it)"
        } else {
            Add-Result "Software" "WARN" "$($e.Name) GPU" "PyTorch cannot use the GPU, so pose and depth analyses run on the CPU (much slower). Run uv sync in analysis (it installs the CUDA build) and check the NVIDIA driver is 580 or later"
        }
    }
}
Remove-Item $probeFile -ErrorAction SilentlyContinue

# Downloaded on first use, so a missing file is not fatal, but that first run
# needs the internet.
$models = @(
    "analysis\rppg\models\face_landmarker.task"
    "analysis\gaze2d\models\face_landmarker.task"
    "analysis\gaze\models\face_landmarker.task"
    "analysis\gaze\models\face_detection_yunet_2023mar.onnx"
    "analysis\expression\models\face_landmarker.task"
    "analysis\expression\models\emotion-ferplus-8.onnx"
    "analysis\facemask\models\face_landmarker.task"
    "analysis\facemask\models\face_detection_yunet_2023mar.onnx"
)
$missingModels = @($models | Where-Object { -not (Test-Path (Join-Path $repo $_)) -or (Get-Item (Join-Path $repo $_)).Length -lt 1024 })
if ($missingModels.Count -eq 0) {
    Add-Result "Software" "PASS" "Analysis models" "$($models.Count) model files present"
} else {
    Add-Result "Software" "WARN" "Analysis models" ("missing: " + ($missingModels -join ", ") + "; they download on first use, which needs the internet")
}
$hfToken = [string](Get-Prop (Get-Prop $settings "analysis" $null) "hf_token" "")
if ($hfToken) {
    Add-Result "Software" "PASS" "Hugging Face token" "set (needed by Speaker Diarization)"
} else {
    Add-Result "Software" "INFO" "Hugging Face token" "not set; only Speaker Diarization needs it (Settings, Analysis)"
}

# ---------------------------------------------------------------------------
# Storage
# ---------------------------------------------------------------------------
Write-Section "Storage"
$recDir = [string](Get-Prop $record "directory" "")
if (-not $recDir) {
    Add-Result "Storage" "WARN" "Recordings folder" "not set in the settings"
} else {
    # A relative folder is relative to where MOSAIC starts: the repository
    # when it is run from a build here.
    if (-not [System.IO.Path]::IsPathRooted($recDir)) { $recDir = Join-Path $repo $recDir }
    $recDir = [System.IO.Path]::GetFullPath($recDir)
    $existing = $recDir
    while ($existing -and -not (Test-Path $existing)) { $existing = Split-Path $existing -Parent }
    if (-not $existing) {
        Add-Result "Storage" "FAIL" "Recordings folder" "$recDir is on a drive that does not exist"
    } else {
        $where = if ($existing -eq $recDir) { $recDir } else { "$recDir (not created yet)" }
        # GetDiskFreeSpaceEx, as Qt uses: it also works for a network
        # folder, which has no PowerShell drive.
        $freeBytes = [double]0
        $free = [UInt64]0; $diskSize = [UInt64]0; $totalFree = [UInt64]0
        if ([MosaicDoctor.Disk]::GetDiskFreeSpaceEx($existing, [ref]$free, [ref]$diskSize, [ref]$totalFree)) {
            $freeBytes = [double]$free
        }
        $drive = (Get-Item $existing).PSDrive
        # The same estimate as the app's own pre-recording check
        # (estimate_recording_bytes_per_sec in src/session/preflight.cpp).
        $rate = $recordingCameras * [double](Get-Prop $video "bitrate" 0) * 1000.0 / 8.0
        if ([bool](Get-Prop $record "enable_audio" $false)) {
            foreach ($mic in @(Get-Prop $audio "microphones" @())) {
                $a = [double](Get-Prop $mic "sample_rate" 48000) * [double](Get-Prop $mic "channels" 1) * 2.0
                if (-not ([string](Get-Prop $audio "codec" "pcm")).StartsWith("pcm")) { $a /= 4.0 }
                $rate += $a
            }
        }
        if ($rate -gt 0) {
            $minutes = $freeBytes / $rate / 60.0
            $detail = "{0}: {1:N0} GB free, about {2:N1} h of recording at {3:N1} MB/s" -f $where, ($freeBytes / 1e9), ($minutes / 60.0), ($rate / 1e6)
            if ($minutes -lt 10) { $level = "FAIL" } elseif ($minutes -lt 60) { $level = "WARN" } else { $level = "PASS" }
            Add-Result "Storage" $level "Free space" $detail
        } else {
            Add-Result "Storage" "INFO" "Free space" ("{0}: {1:N0} GB free" -f $where, ($freeBytes / 1e9))
        }

        try {
            if (-not $drive) { throw "a network folder" }
            $letter = $drive.Name
            $disk = Get-Partition -DriveLetter $letter -ErrorAction Stop | Get-Disk -ErrorAction Stop
            $phys = Get-PhysicalDisk -ErrorAction Stop | Where-Object DeviceId -eq ([string]$disk.Number)
            $media = if ($phys) { "$($phys.MediaType) $($phys.BusType)" } else { "unknown type" }
            $kind = "INFO"
            if ($phys -and $phys.MediaType -eq "HDD") { $kind = "WARN"; $media += "; a spinning disk can stall while the analyses read from it during a recording" }
            Add-Result "Storage" $kind "Drive $($letter):" "$($disk.FriendlyName), $media"
        } catch {
            Add-Result "Storage" "INFO" "Drive" "type unknown ($($_.Exception.Message))"
        }

        if (-not $SkipDiskTest) {
            $testFile = Join-Path $existing ("mosaic_doctor_{0}.tmp" -f [guid]::NewGuid().ToString("N"))
            $block = New-Object byte[] (4MB)
            (New-Object Random).NextBytes($block)
            $total = 256MB
            try {
                $sw = [Diagnostics.Stopwatch]::StartNew()
                $fs = New-Object IO.FileStream($testFile, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None, 4096, [IO.FileOptions]::WriteThrough)
                try {
                    for ($written = 0; $written -lt $total; $written += $block.Length) { $fs.Write($block, 0, $block.Length) }
                    $fs.Flush($true)
                } finally { $fs.Dispose() }
                $sw.Stop()
                $mbps = ($total / 1e6) / $sw.Elapsed.TotalSeconds
                $need = [math]::Max($rate / 1e6, 0.1)
                $detail = "{0:N0} MB/s sustained write; recording needs about {1:N1} MB/s" -f $mbps, $need
                if ($mbps -lt 3 * $need) {
                    Add-Result "Storage" "FAIL" "Write speed" "$detail; record to a faster local drive"
                } elseif ($mbps -lt 10 * $need) {
                    Add-Result "Storage" "WARN" "Write speed" "$detail; little headroom for the analyses or a second program"
                } else {
                    Add-Result "Storage" "PASS" "Write speed" $detail
                }
            } catch {
                Add-Result "Storage" "WARN" "Write speed" "could not write a test file in $existing ($($_.Exception.Message))"
            } finally {
                Remove-Item $testFile -ErrorAction SilentlyContinue
            }
        }
    }
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
$fails = @($results | Where-Object Level -eq "FAIL").Count
$warns = @($results | Where-Object Level -eq "WARN").Count
Write-Host ""
if ($fails -gt 0) {
    Write-Host "$fails failed, $warns warnings." -ForegroundColor Red
} elseif ($warns -gt 0) {
    Write-Host "No failures, $warns warnings." -ForegroundColor Yellow
} else {
    Write-Host "Everything passed." -ForegroundColor Green
}
if ($Json) {
    [PSCustomObject]@{
        computer = $env:COMPUTERNAME
        time     = (Get-Date).ToString("s")
        settings = $settingsPath
        results  = $results
    } | ConvertTo-Json -Depth 4 | Set-Content -Path $Json -Encoding UTF8
    Write-Host "Report written to $Json"
}
exit [int]($fails -gt 0)
