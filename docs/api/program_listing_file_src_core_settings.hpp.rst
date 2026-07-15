
.. _program_listing_file_src_core_settings.hpp:

Program Listing for File settings.hpp
=====================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_core_settings.hpp>` (``src/core/settings.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
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
       bool    reverseY        = false;
       QString pixelFormat     = "BGR8";
       bool    specifyFps      = true;
       double  fps             = 30.0;
   
       // Exposure
       QString exposureAuto        = "Off";    // "Off" | "Once" | "Continuous"
       double  exposureTimeUs      = 10000.0;
       double  exposureAutoLowerUs = 100.0;
       double  exposureAutoUpperUs = 50000.0;
   
       // Gain
       QString gainAuto        = "Off";        // "Off" | "Once" | "Continuous"
       double  gainDb          = 0.0;
       double  gainAutoLowerDb = 0.0;
       double  gainAutoUpperDb = 24.0;
   
       // Processing
       double  gamma            = 1.0;
       double  blackLevel       = 0.0;
       QString balanceWhiteAuto = "Off";       // "Off" | "Once" | "Continuous"
   
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
   
       // Frame rate synchronisation across cameras
       bool    syncFps   = false;
       int     targetFps = 30;
   
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
   
       // Per-device configurations
       std::vector<MicrophoneParameters> microphones;
   
       [[nodiscard]] QJsonObject                 to_json()               const;
       [[nodiscard]] static std::optional<AudioSettings> from_json(const QJsonObject&);
   };
   
   // ── Per-keyboard-trigger configuration ────────────────────────────────────
   struct KeyTriggerConfig {
       QString name    = "Trigger";
       QString keySeq  = "";           // e.g. "F1", "Ctrl+Space" — empty = unbound
       bool    enabled = true;
   
       [[nodiscard]] QJsonObject to_json() const;
       [[nodiscard]] static std::optional<KeyTriggerConfig> from_json(const QJsonObject&);
   };
   
   // ── Parallel port trigger configuration ───────────────────────────────────
   // One entry per LPT port (typically just one: LPT1 at 0x378).
   // Implemented on Windows via InpOut32; a stub is used elsewhere.
   struct ParallelPortConfig {
       QString portAddress = "0x378"; // data register hex address (LPT1 = 0x378)
       int     pollRateMs  = 1;       // polling interval in milliseconds
       bool    enabled     = false;
       bool    invertLogic = false;   // true = active-low (common in TTL circuits)
   
       [[nodiscard]] QJsonObject to_json() const;
       [[nodiscard]] static std::optional<ParallelPortConfig> from_json(const QJsonObject&);
   };
   
   // ── Per-LSL-inlet configuration ────────────────────────────────────────────
   struct LslInletConfig {
       QString name       = "LSL Inlet";
       QString streamName = "Markers";
       QString streamType = "Markers";
       bool    enabled    = true;
   
       [[nodiscard]] QJsonObject to_json() const;
       [[nodiscard]] static std::optional<LslInletConfig> from_json(const QJsonObject&);
   };
   
   // ── Trigger ────────────────────────────────────────────────────────────────
   struct TriggerSettings {
       bool    receiveEnabled   = true;
   
       // LSL outlet — publishes one sample per video frame so other tools can
       // align their data to MOSAIC's timeline.
       bool    lslOutletEnabled = true;
       QString lslOutletName    = "MOSAIC";
       double  lslOutletRate    = 30.0;  // nominal sample rate published to LSL
   
       std::vector<KeyTriggerConfig>      keyboardTriggers;
       std::vector<LslInletConfig>        lslInlets;
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
   
   // ── Application aggregate ──────────────────────────────────────────────────
   struct AppSettings {
       static constexpr int k_schema_version = 1;
   
       VideoSettings   video;
       AudioSettings   audio;
       TriggerSettings trigger;
       RecordSettings  record;
   
       // Persist to / restore from a JSON file.
       // save() returns false only on I/O error (not on validation issues).
       [[nodiscard]] bool                            save(const QString& path) const;
       [[nodiscard]] static std::optional<AppSettings> load(const QString& path);
   
       // Platform-standard config location:  ~/.config/CSRU/mosaic/settings.json
       [[nodiscard]] static QString default_path();
   };
   
   } // namespace mosaic
