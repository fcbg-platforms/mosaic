#pragma once
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <memory>

namespace mosaic {

// ── CameraSync ─────────────────────────────────────────────────────────────

struct CameraSync {
    int index = 0;
    QString videoFile;
    int framesCaptured   = 0;
    double fpsActual     = 0.0;
    double coveragePct   = 0.0; // % of master ticks with a "fresh" frame
    double meanDeltaMs   = 0.0; // mean |timing error| across all ticks
    double maxDeltaMs    = 0.0; // worst-case timing error
    int64_t firstWallNs  = 0;
    int64_t lastWallNs   = 0;
    int64_t seekOffsetMs = 0; // seek position in the video at master t = 0
};

// ── SyncManifest ───────────────────────────────────────────────────────────
//
// Aligns N camera streams to a uniform master timeline using per-frame
// monotonic (elapsed_ns) timestamps stored in timestamps_cam_N.csv.  All
// VideoGrabber threads share one process-wide elapsed_ns() origin, so these
// values are directly comparable across cameras without any cross-camera
// clock reconciliation — unlike wall_ns (system_clock), which can jump if
// the OS clock is adjusted mid-session and is therefore only used here for
// the audio-seek computation, which is inherently wall-clock-relative.
//
// For each master tick the nearest frame in every camera is assigned and
// its signed timing error (delta_ms) is recorded.  The result is stored in
// sync_manifest.json and used by SessionPlayerW for frame-accurate,
// synchronised multi-camera playback.
//
// Usage:
//   auto m = SyncManifest::generate(sessionPath);
//   m.save(sessionPath);
//   ...
//   auto m2 = SyncManifest::load(sessionPath);
//   player->seek(camIdx, m2.seek_offset_ms(camIdx));

class SyncManifest {
   public:
    SyncManifest() = default;

    // ── Factory ───────────────────────────────────────────────────────────
    // Generate from timestamps_cam_N.csv files inside sessionPath.
    // masterFps: the uniform output timeline rate (default 25 fps).
    static SyncManifest generate(const QString& sessionPath, double masterFps = 25.0);

    // Load existing sync_manifest.json from sessionPath.
    static SyncManifest load(const QString& sessionPath);

    // ── Persistence ───────────────────────────────────────────────────────
    bool save(const QString& sessionPath) const;

    // ── Validity ──────────────────────────────────────────────────────────
    [[nodiscard]] bool is_valid() const;

    // ── Timeline metadata ─────────────────────────────────────────────────
    [[nodiscard]] int camera_count() const;
    [[nodiscard]] int total_ticks() const;
    [[nodiscard]] double master_fps() const;
    [[nodiscard]] int64_t duration_ms() const;
    [[nodiscard]] int64_t t_origin_ns() const;      // master t=0 on the elapsed_ns clock
    [[nodiscard]] int64_t t_origin_wall_ns() const; // same instant, wall-clock (for reference)

    // ── Per-camera info ────────────────────────────────────────────────────
    [[nodiscard]] const CameraSync& camera_info(int idx) const;

    // ── Frame lookup ──────────────────────────────────────────────────────
    // frame_id assigned to master tick `tick` for camera `cameraIdx`.
    // Returns -1 if there is no data for that camera.
    [[nodiscard]] int frame_at_tick(int cameraIdx, int tick) const;

    // Signed timing error (ms) for the frame at the given tick.
    // Positive = frame was captured after the tick's ideal time.
    [[nodiscard]] double delta_ms_at_tick(int cameraIdx, int tick) const;

    // Nearest tick index for a master position in ms.
    [[nodiscard]] int tick_for_ms(int64_t posMs) const;

    // How far into the video file the player must seek to be at master t = 0.
    // Positive for cameras that started before t_origin (most cameras).
    [[nodiscard]] int64_t seek_offset_ms(int cameraIdx) const;

    // How far into the audio file the player must seek to be at master t = 0.
    // Derived from the gap between session_start_utc and t_origin.
    [[nodiscard]] int64_t audio_seek_ms() const;

    // Human-readable quality summary for all cameras.
    [[nodiscard]] QString quality_report() const;

   private:
    // Columnar storage: index = cameraIdx * totalTicks_ + tick
    QVector<int> frameIds_;  // -1 = no frame
    QVector<float> deltaMs_; // signed timing error per (camera, tick)

    QVector<CameraSync> cameras_;
    int totalTicks_        = 0;
    double masterFps_      = 25.0;
    int64_t durationMs_    = 0;
    int64_t tOriginNs_     = 0; // master t=0, elapsed_ns clock — alignment key
    int64_t tOriginWallNs_ = 0; // same instant, wall_ns — audio seek only
    int64_t stepNs_        = 0;
    int64_t audioSeekMs_   = 0;
    QString generatedAt_;

    struct FrameTs {
        int frameId       = 0;
        int64_t elapsedNs = 0;
        int64_t wallNs    = 0;
    };
    static QVector<FrameTs> read_timestamps(const QString& csvPath);
    static int64_t session_start_wall_ns(const QString& sessionPath);
};

} // namespace mosaic
