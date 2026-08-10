#pragma once
#include <QImage>
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>
#include <memory>
#include <optional>

namespace mosaic {

// One camera's live pose/gaze dashboard tile for the Real-time tab: a
// thumbnail with a pose-skeleton + gaze overlay (ported from
// PoseOverlayPlayerW/SkeletonOverlayW's QPainter approach and colors, for
// visual consistency with the post-hoc Analysis-tab overlay), a small gaze
// compass, and a footer with a pose-tracking-quality badge and a neutral
// gaze on-target percentage (no per-tile sparkline — the tab's own
// RealtimeTraceW panel is the one place a live trace is shown).
//
// Receives frames/keypoints/gaze data via direct slots — the tab dispatches
// PoseWorker::pose_ready()/gaze_ready() and VideoManager::frame_preview() to
// the matching tile by camera index. See the "Item 22" roadmap plan section
// for the full design rationale (why this bypasses MonitorBridge/QML, why
// there's no live 3D room view, etc).
class RealtimeCameraTileW : public QWidget {
    Q_OBJECT
public:
    explicit RealtimeCameraTileW(int cameraIndex, bool liveAnalysisEnabled,
                                  int detectionWindowBuckets = 24,
                                  QWidget* parent = nullptr);
    ~RealtimeCameraTileW() override;

    [[nodiscard]] int  camera_index() const;
    [[nodiscard]] bool analyze_enabled() const;

    // Aggregate rates for the tab-level KPI strip; empty until at least one
    // observation has been recorded for this tile.
    [[nodiscard]] std::optional<double> detection_rate() const;
    [[nodiscard]] std::optional<double> gaze_on_target_rate() const;

    // Reflects the tab-wide paused (recording) state: desaturates the footer
    // stats so a frozen "92%, green" badge from before the pause doesn't
    // read as live data. Does not affect the thumbnail — it keeps updating
    // during a pause (frame_preview is untouched by the pause).
    void set_analysis_paused(bool paused);

public slots:
    void on_frame_preview(QImage frame);
    void on_pose_ready(QVariantList keypoints);
    void on_gaze_ready(QVariantMap gazeData);

signals:
    // Emitted when the user toggles this tile's own "Analyze" checkbox — the
    // tab connects this straight to CameraParameters::liveAnalysisEnabled.
    void analyze_toggled(int cameraIndex, bool enabled);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
