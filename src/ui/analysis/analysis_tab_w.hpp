#pragma once
#include "analysis/analysis_manager.hpp"
#include "core/settings.hpp"
#include <QWidget>
#include <memory>

namespace mosaic {

// Top-level "Analysis" tab: pick a recorded session, run a post-hoc analysis
// plugin, and review the result in-app.
//
// Two plugins today, selected via a combo box: Pose (YOLOv8) — a synchronised
// video+skeleton-overlay player alongside a per-keypoint metrics plot — and
// Face Masking — plain playback of an anonymized output video written to a
// sibling "anonymized/" folder, never touching the original recording.
// AnalysisManager (analysis/analysis_manager.hpp) runs either plugin's script
// through the same shared subprocess queue.
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
    void select_plugin(int index);
    void run_analysis();
    void reload_current_camera_result();
    void open_output_folder();
    [[nodiscard]] bool is_pose_plugin() const;
    [[nodiscard]] QString pose_json_path_for(const QString& videoRelPath) const;
    [[nodiscard]] QString anonymized_video_path_for(const QString& videoRelPath) const;

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
