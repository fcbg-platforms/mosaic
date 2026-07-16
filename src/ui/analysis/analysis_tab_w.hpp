#pragma once
#include "analysis/analysis_manager.hpp"
#include "core/settings.hpp"
#include <QWidget>
#include <memory>

namespace mosaic {

// Top-level "Analysis" tab: pick a recorded session, run a post-hoc analysis
// plugin with a chosen model, and review the result in-app — a synchronised
// video+skeleton-overlay player alongside a per-keypoint metrics plot. The
// plot can show raw Position (x/y) or, entirely computed client-side from
// the already-loaded result (no extra Python run needed — see
// analysis/pose_kinematics.hpp), derived Speed/Acceleration with optional
// smoothing and an optional manual px-to-mm scale.
//
// Scoped to the Pose (YOLOv8) plugin today; the plugin selector is a combo
// box rather than hardcoded UI so a second plugin doesn't need a UI
// redesign here — but AnalysisManager itself (analysis/analysis_manager.hpp)
// is still hardcoded to run_pose.py's argument shape, so adding a real
// second plugin also means extending AnalysisManager, not just this combo.
class AnalysisTabW : public QWidget {
    Q_OBJECT
public:
    explicit AnalysisTabW(AppSettings&     settings,
                          AnalysisManager* analysisMgr,
                          QWidget*         parent = nullptr);
    ~AnalysisTabW() override;

private:
    void build_ui();
    void rebuild_session_list();
    void select_session(const QString& path);
    void select_camera(int index);
    void run_analysis();
    void reload_current_camera_result();
    void update_kinematics_chart();
    void export_kinematics_csv();
    [[nodiscard]] QString pose_json_path_for(const QString& videoRelPath) const;

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
