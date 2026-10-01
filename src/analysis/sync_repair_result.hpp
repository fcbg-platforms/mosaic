#pragma once
#include <QPair>
#include <QString>
#include <QVector>
#include <cstdint>

namespace mosaic {

/// One camera's Frame Sync Repair outcome. Mirrors run_sync_repair.py's
/// "cameras" JSON array exactly. sourceVideo/repairedVideo are empty when
/// skipped == true (this camera never produced a repaired copy).
struct SyncRepairCamera {
    int index = 0;
    QString sourceVideo;   // "video/video_N.mp4", empty if skipped
    QString repairedVideo; // "synced/video_N.mp4", empty if skipped
    int sourceFramesCaptured = 0;
    int outputFrameCount     = 0;
    int duplicatedFrameCount = 0;
    bool skipped             = false;
    QString skipReason; // empty unless skipped
    QString note;       // empty unless run_sync_repair.py's truncated-source-video
                        // guard fired for this camera (see run_sync_repair.py's
                        // _repair_camera() doc comment)

    // ── How it was aligned, and what it is missing ─────────────────────────
    // Absent from reports written before trigger-tick alignment existed;
    // those read as arrival-time alignment with nothing more known.

    /// "trigger_ticks:hw_timestamp", "trigger_ticks:arrival_interval" or
    /// "arrival_time". Empty in older reports.
    QString alignment;
    /// Output frames with no real frame for their tick — shown as the last
    /// real frame with a red MISSING tag. -1 when the report predates it
    /// (duplicatedFrameCount is the older, equivalent figure).
    int missingFrameCount = -1;
    int gapCount          = 0;
    /// Inclusive output-frame ranges [first, last]; at most 100 are listed
    /// (gapCount has the total).
    QVector<QPair<int, int>> gaps;
    /// This camera's frames before/after the window every camera was
    /// running in; -1 when unknown (arrival-time alignment).
    int leadInTrimmed = -1;
    int tailTrimmed   = -1;
    /// Its trigger-to-arrival latency did not settle near the other
    /// cameras' — its placement on ticks may be off by one.
    bool alignmentUncertain = false;
};

/// Parses a "synced/sync_repair.json" summary written by
/// analysis/run_sync_repair.py into a queryable in-memory structure, for
/// the Analysis tab's Frame Sync Repair plugin. Deliberately does NOT parse
/// the (much larger) per-camera "synced/video_N.repair_map.csv" audit
/// files — nothing in the UI needs frame-level detail, only this small
/// per-camera summary; the CSVs are for external/advanced use.
///
/// Usage:
/// @code
///   auto result = SyncRepairResult::load(jsonPath);
///   if (result.is_valid()) { ... }
/// @endcode
class SyncRepairResult {
   public:
    SyncRepairResult() = default;

    /// Parses jsonPath. Returns a default-constructed (is_valid() == false)
    /// result if the file is missing or malformed.
    static SyncRepairResult load(const QString& jsonPath);

    [[nodiscard]] bool is_valid() const { return valid_; }
    [[nodiscard]] double master_fps() const { return masterFps_; }
    [[nodiscard]] int total_ticks() const { return totalTicks_; }
    [[nodiscard]] int64_t duration_ms() const { return durationMs_; }
    /// "trigger_ticks" or "arrival_time"; empty in reports that predate it.
    [[nodiscard]] const QString& alignment() const { return alignment_; }
    [[nodiscard]] bool on_trigger_ticks() const { return alignment_ == "trigger_ticks"; }
    [[nodiscard]] const QVector<SyncRepairCamera>& cameras() const { return cameras_; }

    /// Sum of duplicatedFrameCount across every non-skipped camera.
    [[nodiscard]] int total_duplicated_frames() const;

    /// Count of cameras with skipped == true.
    [[nodiscard]] int skipped_camera_count() const;

   private:
    bool valid_         = false;
    double masterFps_   = 0.0;
    int totalTicks_     = 0;
    int64_t durationMs_ = 0;
    QString alignment_;
    QVector<SyncRepairCamera> cameras_;
};

} // namespace mosaic
