#pragma once
#include "trigger/trigger_types.hpp"
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <array>
#include <optional>
#include <vector>

namespace mosaic {

// Each subsystem owns its settings struct. All structs must be trivially
// default-constructible so AppSettings{} always produces safe defaults.

// ── Camera intrinsic + extrinsic calibration ───────────────────────────────
// Populated by CalibrationManager after a checkerboard calibration run.
// Stored per camera so it survives between sessions.
struct CalibrationData {
    bool   calibrated = false;
    double rmsError   = -1.0;   // reprojection error in pixels; -1 = uncalibrated

    // Camera matrix (3×3, row-major): [fx 0 cx; 0 fy cy; 0 0 1]
    std::array<double, 9>  cameraMatrix = {1,0,0, 0,1,0, 0,0,1};
    // Distortion coefficients: k1, k2, p1, p2, k3
    std::array<double, 5>  distCoeffs   = {};
    // 4×4 homogeneous RT relative to camera 0 (row-major).
    // Camera 0 always has identity here.
    std::array<double, 16> extrinsicRt  = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    // Populated by RoomCalibrationManager (multi-camera room/extrinsic
    // calibration) — distinct from `calibrated`, which only ever means
    // "intrinsics done". extrinsicRt stays at its default identity while
    // this is false.
    bool extrinsicCalibrated = false;

    [[nodiscard]] QJsonObject to_json() const;
    [[nodiscard]] static std::optional<CalibrationData> from_json(const QJsonObject&);
};

// ── Per-camera parameters ──────────────────────────────────────────────────
struct CameraParameters {
    // Identity (filled by the camera backend when a real device is connected)
    QString serialNumber;
    QString friendlyName = "Camera";

    // Image / Acquisition
    int     width           = 1920;
    int     height          = 1080;
    int     offsetX         = 0;
    int     offsetY         = 0;
    bool    reverseX        = false;
    // No longer offered in CameraCardW — confirmed on real room-11
    // hardware that this camera generation's ReverseY node exists but
    // rejects writes outright ("Node is not writable", every camera, every
    // session). Field kept so an already-persisted value round-trips
    // rather than silently vanishing on next save.
    bool    reverseY        = false;
    QString pixelFormat     = "BGR8";
    bool    specifyFps      = true;
    double  fps             = 25.0;  // acA1920-25gc's max sustained rate

    // Exposure. Defaults to "Once" rather than "Off": this camera generation
    // (acA1920-25gc) has no working manual-gain path (see gainAuto below),
    // and auto-calibrating once at preview start then holding steady for
    // the whole recording gives a scene-matched image without the
    // brightness/color drift "Continuous" can introduce mid-recording —
    // real risk for anything downstream that assumes consistent lighting
    // frame-to-frame (e.g. expression/gaze confidence, luminance measures).
    QString exposureAuto        = "Once";   // "Off" | "Once" | "Continuous"
    double  exposureTimeUs      = 10000.0;
    double  exposureAutoLowerUs = 100.0;
    double  exposureAutoUpperUs = 50000.0;

    // Gain. Defaults to "Once", not "Off": this camera generation only
    // exposes the older SFNC 1.x "GainRaw" node, not the modern "Gain" (dB)
    // node — manual/"Off" gain is a silent no-op here (see the "Gain" write
    // in video_grabber.cpp's apply_image_params(), which only fires when
    // gainAuto=="Off" and simply skips if the modern node isn't present).
    // CameraCardW no longer offers "Off" or a manual gainDb field for
    // exactly this reason — gainDb is kept so an already-persisted value
    // round-trips, but has no effect on this hardware.
    QString gainAuto        = "Once";       // "Off" | "Once" | "Continuous" (UI: Once/Continuous only)
    double  gainDb          = 0.0;
    double  gainAutoLowerDb = 0.0;
    double  gainAutoUpperDb = 24.0;

    // Processing
    double  gamma            = 1.0;
    double  blackLevel       = 0.0;
    QString balanceWhiteAuto = "Once";      // "Off" | "Once" | "Continuous"
    // Manual Red/Blue balance gains, only applied when balanceWhiteAuto ==
    // "Off". Neutral default (1.0 = no correction, matching the camera's
    // own factory-default reference value) until tuned by hand while
    // watching the live preview — "Once" re-converges fresh on every
    // camera open with no readback/persistence anywhere in this codebase,
    // so the resulting tint can drift session to session; "Off" + these
    // fixed ratios makes color reproducible across restarts.
    double  balanceRatioRed  = 1.0;
    double  balanceRatioBlue = 1.0;
    // Saturation/contrast/brightness have no GenICam node on this camera
    // generation at all (no on-camera ISP) — CameraCardW no longer offers
    // them. Fields kept for persistence/forward-compat only (a future
    // camera model might expose the matching node).
    double  saturation       = 1.0;         // 0.0–2.0
    double  contrast         = 1.0;         // 0.0–2.0
    double  brightness       = 0.5;         // 0.0–1.0
    double  autoTargetBrightness = 0.5;     // 0.0–1.0
    int     digitalShift     = 0;           // 0–4 bits (12→8 or 16→8 conversions)

    // Hardware trigger input (e.g. TTL pulse on Basler Line1, or a GigE
    // Vision Action Command broadcast when set to "Action1" — see
    // gige_action_command.hpp). Defaults to Action1 enabled: room 11's
    // camera fleet is meant to be synchronized this way out of the box for
    // any newly-added camera, without a manual per-camera opt-in step.
    bool    hwTriggerEnabled = true;
    QString hwTriggerSource  = "Action1";   // "Line1" | "Line2" | "Line3" | "Software" | "Action1"
    double  hwTriggerDelayUs = 0.0;         // microseconds

    // Live pose/gaze analysis (Real-time tab) opt-out for this camera. The
    // live pipeline shares one MediaPipe subprocess across every camera at a
    // fixed ~2fps/camera budget (see MainWindow's throttle lambda) — turning
    // this off for a camera the user doesn't care about live frees up that
    // camera's slice of the shared budget without touching recording at all.
    bool    liveAnalysisEnabled = true;

    // Test pattern — shown in monitor when no real camera is connected
    QString testPattern      = "Off";       // "Off" | "ColorBars" | "Horizontal" | "Vertical"

    // Calibration (filled by CalibrationManager after a checkerboard run)
    CalibrationData calibration;

    [[nodiscard]] QJsonObject to_json() const;
    [[nodiscard]] static std::optional<CameraParameters> from_json(const QJsonObject&);
};

// ── Video ──────────────────────────────────────────────────────────────────
struct VideoSettings {
    // Global encoding
    QString codec    = "h264_nvenc";    // "h264_nvenc" | "hevc_nvenc" | "libx264"
    QString preset   = "p4";            // GPU: p1-p7  |  CPU: fast, medium, slow
    int     crf      = 23;              // quality for CPU encoder (17=best, 28=worst)
    int     bitrate  = 5000;            // kbit/s (GPU encoder)

    // Upper bound on the number of configured cameras this app will ever
    // treat as a live rig (room 11 has 6; 32 is a generous safety margin,
    // not a hardware limit). Every code path that populates `cameras`
    // before a VideoManager::open() call — VideoSettings::from_json() and
    // Application::initialize()'s programmatic room-11 seed — must reserve
    // at least this much capacity first. VideoManager::open() binds a raw
    // `const CameraParameters&` into each VideoGrabber for that grabber's
    // entire lifetime (see VideoGrabber::Impl::params in video_grabber.cpp);
    // any reallocation of this vector after that point (e.g. from
    // push_back() growing past capacity) silently invalidates every such
    // reference, causing a use-after-free the next time a camera control is
    // edited. VideoSettingsW's constructor also reserves this same cap for
    // the identical reason on behalf of CameraCardW's references.
    static constexpr int kMaxCameras = 32;

    // Per-camera configurations (one entry per added camera)
    std::vector<CameraParameters> cameras;

    [[nodiscard]] QJsonObject                        to_json()               const;
    [[nodiscard]] static std::optional<VideoSettings> from_json(const QJsonObject&);
};

// ── Per-microphone parameters ──────────────────────────────────────────────
struct MicrophoneParameters {
    QString deviceId;                   // OS device identifier
    QString friendlyName = "Microphone";
    int     sampleRate   = 44100;       // Hz
    int     channels     = 2;
    int     bufferSize   = 512;         // frames per callback

    [[nodiscard]] QJsonObject to_json() const;
    [[nodiscard]] static std::optional<MicrophoneParameters> from_json(const QJsonObject&);
};

// ── Audio ──────────────────────────────────────────────────────────────────
struct AudioSettings {
    // Global encoding applied to all recordings
    QString codec = "pcm_s16le";        // "pcm_s16le" | "flac" | "aac" | "mp3"

    // Upper bound on the number of configured microphones this app will ever
    // treat as a live monitoring/recording rig — mirrors VideoSettings::
    // kMaxCameras exactly, see its doc comment for the full use-after-free
    // rationale. AudioManager::start_monitoring()/start() bind a raw
    // `const MicrophoneParameters&` into each AudioRecorder for that
    // recorder's entire lifetime (see AudioRecorder::m_params), and
    // AudioSettingsW's constructor reserve()s this same cap against the
    // same vector on behalf of MicrophoneCardW's references. 16 matches the
    // reserve(16) call AudioSettingsW's constructor already made before
    // this fix (a literal, not a hardware limit) — no known rig needs
    // anywhere close to this many microphones.
    static constexpr int kMaxMicrophones = 16;

    // Per-device configurations
    std::vector<MicrophoneParameters> microphones;

    [[nodiscard]] QJsonObject                 to_json()               const;
    [[nodiscard]] static std::optional<AudioSettings> from_json(const QJsonObject&);
};

// ── Per-keyboard-trigger configuration ────────────────────────────────────
struct KeyTriggerConfig {
    QString       name    = "Trigger";
    QString       keySeq  = "";            // e.g. "F1", "Ctrl+Space" — empty = unbound
    bool          enabled = true;
    TriggerAction action  = TriggerAction::Log;

    [[nodiscard]] QJsonObject to_json() const;
    [[nodiscard]] static std::optional<KeyTriggerConfig> from_json(const QJsonObject&);
};

// ── Serial port trigger configuration ─────────────────────────────────────
// Receives trigger events via RS-232/USB-serial.  Uses Qt6::SerialPort.
// A trigger fires when an incoming byte satisfies the match rule.
struct SerialTriggerConfig {
    QString       name       = "Serial Trigger";
    QString       portName   = "";         // "COM3", "/dev/ttyUSB0" — empty = disabled
    int           baudRate   = 9600;
    int           dataBits   = 8;
    QString       parity     = "None";     // "None" | "Even" | "Odd"
    double        stopBits   = 1.0;        // 1.0 | 1.5 | 2.0
    // matchMode controls what incoming bytes trigger an event:
    //   "AnyByte" — any received byte fires the trigger
    //   "NonZero" — only non-zero bytes fire
    //   "Exact"   — only bytes matching matchValue (hex: "0xFF") fire
    QString       matchMode  = "AnyByte";
    QString       matchValue = "";
    bool          enabled    = false;
    TriggerAction action     = TriggerAction::Log;

    [[nodiscard]] QJsonObject to_json() const;
    [[nodiscard]] static std::optional<SerialTriggerConfig> from_json(const QJsonObject&);
};

// ── Parallel port trigger configuration ───────────────────────────────────
// One entry per LPT port (typically just one: LPT1 at 0x378).
// Implemented on Windows via InpOut32; a stub is used elsewhere.
struct ParallelPortConfig {
    QString       portAddress = "0x378";   // data register hex address (LPT1 = 0x378)
    int           pollRateMs  = 1;         // polling interval in milliseconds
    bool          enabled     = false;
    bool          invertLogic = false;     // true = active-low (common in TTL circuits)
    TriggerAction action      = TriggerAction::Log;

    // Drives the Control register's INIT pin (bit 2, portAddr+2 — physically
    // separate from the Data register this class reads, so no conflict) high
    // when a recording starts and low when it stops, so an external device
    // listening on that pin (e.g. an EEG amplifier) logs a marker for
    // MOSAIC's own recording start/stop. See ParallelPortTrigger::
    // set_recording_marker().
    bool          sendRecordingMarker = false;

    [[nodiscard]] QJsonObject to_json() const;
    [[nodiscard]] static std::optional<ParallelPortConfig> from_json(const QJsonObject&);
};

// ── Trigger ────────────────────────────────────────────────────────────────
struct TriggerSettings {
    bool    receiveEnabled   = true;

    std::vector<KeyTriggerConfig>      keyboardTriggers;
    std::vector<SerialTriggerConfig>   serialTriggers;
    std::vector<ParallelPortConfig>    parallelPorts;

    [[nodiscard]] QJsonObject                        to_json()               const;
    [[nodiscard]] static std::optional<TriggerSettings> from_json(const QJsonObject&);
};

// ── Record ─────────────────────────────────────────────────────────────────
struct RecordSettings {
    QString directory       = "./recordings";
    QString videoBasename   = "video";
    QString audioBasename   = "audio";
    QString triggerBasename = "trigger";
    QString separator       = "_";
    bool    addTimestamp    = true;
    QString timestampFormat = "yyyy-MM-dd_hh-mm-ss";

    // Which channels to record
    bool enableVideo   = true;
    bool enableAudio   = true;
    bool enableTrigger = true;

    [[nodiscard]] QJsonObject                  to_json()               const;
    [[nodiscard]] static std::optional<RecordSettings> from_json(const QJsonObject&);
};

struct AnalysisSettings {
    // Hugging Face access token for the gated pyannote speaker-diarization
    // models (Analysis tab's Speaker Diarization plugin). Persisted here
    // (rather than kept transient/re-entered per session) so the user
    // doesn't have to re-paste it every run — the tradeoff being that it's
    // stored in plaintext in this profile's settings.json, same as every
    // other field in AppSettings.
    QString hfToken;

    [[nodiscard]] QJsonObject                     to_json()   const;
    [[nodiscard]] static std::optional<AnalysisSettings> from_json(const QJsonObject&);
};

// ── Room (multi-camera extrinsic calibration) settings ─────────────────────
// The room's shared reference plane, defined once via RoomCalibrationW's
// "Use last shot as plane" action (the ChArUco board is laid flat on the
// target surface for one shot). Consumed by analysis/run_gaze_fusion.py via
// the "room" section RecordManager writes into session_meta.json — a plane
// isn't camera-specific, so it lives here rather than inside CalibrationData.
struct RoomSettings {
    std::array<double, 3> planePoint  = {0, 0, 0};
    std::array<double, 3> planeNormal = {0, 0, 1};
    bool                  planeDefined = false;

    [[nodiscard]] QJsonObject                 to_json()   const;
    [[nodiscard]] static std::optional<RoomSettings> from_json(const QJsonObject&);
};

// ── Real-time tab settings ──────────────────────────────────────────────────
// Preferences for the live pose/gaze analytics dashboard (Real-time tab).
// Per-camera opt-out lives on CameraParameters::liveAnalysisEnabled instead
// of a field here, so it travels correctly with its camera on reorder/removal.
struct RealtimeSettings {
    bool enabled                  = true;   // show live analysis in the tab at all
    bool autoPauseDuringRecording = true;   // pause pose/gaze inference while recording
    int  detectionWindowBuckets   = 24;     // sparkline rolling-window bucket count (~2min @ 5s/bucket)

    [[nodiscard]] QJsonObject                     to_json()   const;
    [[nodiscard]] static std::optional<RealtimeSettings> from_json(const QJsonObject&);
};

// ── Application aggregate ──────────────────────────────────────────────────
struct AppSettings {
    static constexpr int k_schema_version = 1;

    VideoSettings    video;
    AudioSettings    audio;
    TriggerSettings  trigger;
    RecordSettings   record;
    AnalysisSettings analysis;
    RoomSettings     room;
    RealtimeSettings realtime;

    // Persist to / restore from a JSON file.
    // save() returns false only on I/O error (not on validation issues).
    [[nodiscard]] bool                            save(const QString& path) const;
    [[nodiscard]] static std::optional<AppSettings> load(const QString& path);

    // Platform-standard config location:  ~/.config/CSRU/mosaic/settings.json
    [[nodiscard]] static QString default_path();
};

} // namespace mosaic
