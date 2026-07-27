#pragma once
#include "analysis/analysis_manager.hpp"
#include "core/settings.hpp"
#include <QWidget>
#include <cstdint>
#include <memory>

namespace mosaic {

// Top-level "Analysis" tab: pick a recorded session, run a post-hoc analysis
// plugin, and review the result in-app.
//
// Five plugins today, selected via a combo box: Pose (YOLOv8) — a
// synchronised video+skeleton-overlay player alongside a per-keypoint
// metrics plot; Face Masking — plain playback of an anonymized output video
// written to a sibling "anonymized/" folder, never touching the original
// recording; Speaker Diarization — faster-whisper transcription plus
// (when a Hugging Face token is available) pyannote.audio speaker labeling
// of each session's audio, shown as a playback-synced transcript table;
// Facial Expression — MediaPipe FaceLandmarker blendshapes classified into a
// dominant basic-emotion label per detected face (via a rule-based heuristic
// or a pretrained FER+ ONNX model), shown as a bbox+label video overlay plus
// a per-blendshape score-over-time plot; and Multi-Camera Gaze Fusion —
// per-camera 3D gaze rays (analysis/gaze) triangulated across whichever
// cameras have both intrinsic and extrinsic (room) calibration, shown as a
// per-camera bbox+direction-arrow video overlay plus a top-down 3D room
// view (GazeRoomViewW). AnalysisManager (analysis/analysis_manager.hpp)
// runs each plugin's script through the same shared subprocess queue. For
// Pose, the metrics plot can show raw Position (x/y) or, entirely computed
// client-side from the already-loaded result (no extra Python run needed —
// see analysis/pose_kinematics.hpp), derived Speed/Acceleration with
// optional smoothing and an optional manual px-to-mm scale.
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
    void update_kinematics_chart();
    void export_kinematics_csv();
    void open_output_folder();
    void update_transcript_table();
    void export_transcript_csv();
    void highlight_active_transcript_row(int64_t ms);
    void update_expression_view();
    void export_expression_csv();
    void update_gaze_view();
    void export_gaze_csv();
    [[nodiscard]] bool is_pose_plugin() const;
    [[nodiscard]] bool is_diarize_plugin() const;
    [[nodiscard]] bool is_expression_plugin() const;
    [[nodiscard]] bool is_face_mask_plugin() const;
    [[nodiscard]] bool is_gaze_fusion_plugin() const;
    [[nodiscard]] QString pose_json_path_for(const QString& videoRelPath) const;
    [[nodiscard]] QString anonymized_video_path_for(const QString& videoRelPath) const;
    [[nodiscard]] QString transcript_json_path_for(const QString& audioRelPath) const;
    [[nodiscard]] QString expression_json_path_for(const QString& videoRelPath) const;

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
