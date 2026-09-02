#pragma once
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
    QVector<SyncRepairCamera> cameras_;
};

} // namespace mosaic
