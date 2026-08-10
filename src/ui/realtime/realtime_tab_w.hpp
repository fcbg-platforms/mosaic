#pragma once
#include "analysis/pose_worker.hpp"
#include "audio/audio_manager.hpp"
#include "core/settings.hpp"
#include "record/record_manager.hpp"
#include "video/video_manager.hpp"
#include <QWidget>
#include <memory>

namespace mosaic {

// Top-level "Real-time" tab: a live pose/gaze analytics dashboard built
// entirely on the pipeline the app already runs (PoseWorker's shared
// MediaPipe subprocess, ~2fps/camera) — no new inference, just a much
// richer view of the same data than the Live tab's hidden overlay toggles.
//
// Bypasses MonitorBridge/QML entirely — this tab is pure QWidgets, wired
// directly to VideoManager::frame_preview / PoseWorker::pose_ready /
// PoseWorker::gaze_ready / AudioManager::envelope_changed via constructor
// injection (same pattern AnalysisTabW uses for AnalysisManager*).
//
// Shows: a KPI strip (cameras active, avg pose detection %, avg gaze
// on-target %, live/paused state), a live position trace for one
// user-selected camera/keypoint (RealtimeTraceW — the real-time counterpart
// to the Analysis tab's post-hoc Position chart), one RealtimeCameraTileW
// per configured camera, and a shared live audio waveform strip. Pose/gaze
// inference
// auto-pauses while a recording is in progress (see MainWindow's
// RecordManager::recording_started/stopped -> PoseWorker::set_paused()
// wiring) — this tab only reflects that state, it doesn't own it, since
// pausing during recording is a global resource policy that must hold
// regardless of which tab is open.
//
// Deliberately has no live 3D/floorplan room view: there is no live
// cross-camera time-sync or calibration-aware fusion pipeline (that only
// exists post-hoc, see GazeRoomViewW/Skeleton3DRoomViewW), so a live room
// map would have to fabricate camera positions. The KPI strip plus each
// tile's own small 2D gaze compass is the honest room-level summary.
class RealtimeTabW : public QWidget {
    Q_OBJECT
public:
    explicit RealtimeTabW(AppSettings&   settings,
                          VideoManager*  videoMgr,
                          AudioManager*  audioMgr,
                          RecordManager* recordMgr,
                          PoseWorker*    poseWorker,
                          QWidget*       parent = nullptr);
    ~RealtimeTabW() override;

    // Restores the tab's internal splitter (tile grid vs. audio strip) to
    // its default proportions — called from MainWindow's View -> Reset
    // layout action.
    void reset_layout();

private:
    void build_ui();
    void rebuild_tiles();
    void rebuild_trace_camera_combo();
    void on_pose_ready_for_trace(int camIdx, const QVariantList& keypoints);
    void on_tick();
    void on_recording_started(const QString& sessionPath);
    void on_recording_stopped(const QString& sessionPath, int durationMs);

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
