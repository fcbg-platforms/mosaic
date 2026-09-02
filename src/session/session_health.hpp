#pragma once
#include <QString>
#include <QVector>
#include <optional>

#include "calibration/rms_quality.hpp"

namespace mosaic {

// One camera's already-gathered raw numbers for a just-finished recording —
// plain data, with no dependency on VideoManager/VideoGrabber, so
// build_session_health_report() is directly unit-testable with hand-built
// fixtures. The caller (MainWindow::show_session_health()) fills this in from
// VideoManager::last_recording_snapshot() plus last_recording_action_ticks()
// and a SyncManifest lookup — deliberately the post-recording snapshots, not
// the live camera_stats()/action_ticks_fired() reads, which the preview
// restart following recording_stopped has already zeroed by then.
struct CameraHealthInput {
    int index = 0;
    QString name;

    // False for a camera that was configured but never actually opened for
    // this recording — a duplicate/ambiguous serial, a failed open(), or a
    // dead cable/NIC link (a real, recurring failure mode on this rig). Such a
    // camera has no counters at all, so every numeric field below stays at its
    // default and must NOT be read as "captured nothing, cleanly" — hence the
    // explicit flag rather than inferring absence from a zero counter.
    bool participated = true;

    int64_t framesGrabbed    = 0;
    int64_t framesEncoded    = 0;
    int64_t framesDropped    = 0; // ring-buffer overflow
    int64_t incompleteFrames = 0; // GVSP packet loss
    double configuredFps     = 0.0;
    double achievableFps     = -1.0; // -1 = not yet measured

    // nullopt if this camera didn't use Action1 (GigE Vision Action
    // Command) triggering this session — see VideoGrabber::action_command_ready().
    std::optional<int64_t> actionTicksFired;

    // nullopt if SyncManifest::generate() produced no entry for this camera
    // (e.g. it captured zero frames, or sync data wasn't generated at all).
    std::optional<double> syncCoveragePct;
    std::optional<double> syncMeanDeltaMs;
    std::optional<double> syncMaxDeltaMs;
};

struct CameraHealthEntry {
    CameraHealthInput raw;

    // max(0, actionTicksFired - framesGrabbed). nullopt iff
    // raw.actionTicksFired is nullopt (Action1 triggering wasn't used).
    std::optional<int64_t> missedTriggerFrames;

    RmsQuality quality = RmsQuality::Excellent;
};

struct SessionHealthReport {
    QString sessionPath;
    QString sessionName;
    int64_t durationMs = 0;
    QVector<CameraHealthEntry> cameras;
    RmsQuality overallQuality = RmsQuality::Excellent; // worst tier across all cameras
    QString headline;                                  // e.g. "3/3 cameras clean"
};

// Buckets one camera's health from its raw counters:
//   - Poor if the camera never opened for this recording (!participated), and
//     Poor if it opened but captured zero frames. Both are checked first: such
//     a camera has no drop/incomplete/sync data to be "bad" in, and its
//     achievableFps stays at the "not measured" sentinel, so it would
//     otherwise fall through every check below and grade Excellent — the exact
//     inversion that made a disconnected camera outrank a healthy one.
//   - Poor if there's any ring-buffer drop or GVSP packet loss at all (both
//     mean lost data, not a fuzzy tradeoff), sync coverage is badly low
//     (<80%), or the missed-trigger gap exceeds the same threshold (5)
//     ActionCommandTicker's own log-warning already uses for "worth flagging".
//   - Acceptable for a borderline sync coverage (<95%) or a real-but-smaller
//     achievable-fps shortfall (<90% of configured).
//   - Good for a smaller still achievable-fps shortfall (<98% of configured).
//   - Excellent otherwise.
// Mirrors rppg_quality_for()'s "hard override for a structural problem,
// graded tiers otherwise" shape (src/analysis/realtime_metrics.cpp).
// Thresholds are documented, reasoned defaults, not derived from a
// calibration study — same honest caveat rppg_quality_for()'s own doc
// comment already carries for its own thresholds.
[[nodiscard]] RmsQuality camera_health_quality_for(const CameraHealthInput& raw,
                                                   std::optional<int64_t> missedTriggerFrames);

// Pure aggregation — see CameraHealthInput's own doc comment for why this
// takes plain data rather than live VideoManager/VideoGrabber objects.
[[nodiscard]] SessionHealthReport build_session_health_report(
    const QString& sessionPath, const QString& sessionName, int64_t durationMs,
    const QVector<CameraHealthInput>& cameras);

} // namespace mosaic
