#pragma once
#include <QVector>
#include <QWidget>
#include <cstdint>
#include <memory>

#include "analysis/analysis_manager.hpp"
#include "core/settings.hpp"

namespace mosaic {

// Top-level "Analysis" tab: pick a recorded session, run a post-hoc analysis
// plugin, and review the result in-app.
//
// Nine plugins today, selected via a combo box: Pose (YOLOv8) — a
// synchronised video+skeleton-overlay player alongside a per-keypoint
// metrics plot; Face Masking — plain playback of an anonymized output video
// written to a sibling "anonymized/" folder, never touching the original
// recording; Speaker Diarization — faster-whisper transcription plus
// (when a Hugging Face token is available) pyannote.audio speaker labeling
// of each session's audio, shown as a playback-synced transcript table;
// Facial Expression — MediaPipe FaceLandmarker blendshapes classified into a
// dominant basic-emotion label per detected face, via one of three
// backends: a rule-based heuristic, a pretrained FER+ ONNX model, or
// py-feat's Detectorv1 (which additionally reports 20 FACS Action Unit
// intensities), shown as a bbox+label video overlay plus a per-blendshape
// score-over-time plot; Multi-Camera Gaze Fusion —
// per-camera 3D gaze rays (analysis/gaze) triangulated across whichever
// cameras have both intrinsic and extrinsic (room) calibration, shown as a
// per-camera bbox+direction-arrow video overlay plus a top-down 3D room
// view (GazeRoomViewW); and 3D Pose Reconstruction — the already-computed
// Pose plugin output (per camera) triangulated across cameras with real
// cross-camera person association (multi-person capable, unlike Gaze
// Fusion's single-subject design), shown as a per-camera reprojected
// skeleton overlay plus an interactive, orbit-rotatable 3D room view
// (Skeleton3DRoomViewW) — the same already-loaded result additionally
// drives a Dyad Analysis panel (analysis/dyadic_kinematics.hpp) deriving
// interpersonal distance, approach/retreat rate, torso-facing similarity,
// and movement-synchrony ("congruent motion") between two picked tracked
// people, purely client-side from the reconstruction already on screen (no
// extra Python run needed), shown as a metric-over-time chart plus a live
// distance readout drawn directly in the 3D room view; EEG/Trigger ↔ Frame Sync — resolves every
// event in the session's trigger.csv (e.g. an EEG amplifier's parallel-port trigger cable) to its
// nearest frame in every camera (analysis/trigger_frame_map.hpp), shown as a click-to-seek table,
// computed synchronously in C++ (no subprocess — unlike every other plugin here, this is pure fast
// CSV-to-CSV arithmetic with no ML dependency, the same shape as SyncManifest's own synchronous
// generate()); and Remote Heart Rate (rPPG) — an EXPERIMENTAL, research-grade-only camera-based
// heart-rate estimate (analysis/rppg/, classical Green/CHROM/POS signal-processing algorithms, no
// deep learning), shown as a BPM-over-time chart plus a per-camera face-ROI debug overlay, a stats
// readout, and a persistent on-screen disclaimer — not a medical device, not clinically validated;
// no blood-pressure or heart-rate- variability estimate is attempted (see item 21's plan section
// for why both were deliberately descoped); and 2D Gaze (calibration-free) — the same normalized
// [-1,1] iris-offset gaze heuristic already running live in the Real-time tab (a third independent
// copy, per this project's own documented cross-plugin duplication convention — see
// analysis/gaze2d/estimator.py's module docstring), needing NO camera
// intrinsic or room/extrinsic calibration at all (unlike Multi-Camera Gaze
// Fusion), shown as a per-frame dx/dy/magnitude chart plus a per-camera
// bbox+direction-arrow video overlay and a stats readout. AnalysisManager
// (analysis/analysis_manager.hpp) runs each ML-backed plugin's script through the same shared
// subprocess queue. For Pose, the metrics plot can show raw Position (x/y) or, entirely computed
// client-side from the already-loaded result (no extra Python run needed — see
// analysis/pose_kinematics.hpp), derived Speed/Acceleration with optional smoothing and an optional
// manual px-to-mm scale.
class AnalysisTabW : public QWidget {
    Q_OBJECT
   public:
    // extraDirectories: additional session-root directories to scan and
    // merge alongside settings.record.directory — per-user recording
    // access control (item 27), same convention as SessionBrowserW's own
    // extraDirectories parameter (empty for a regular user, every other
    // known profile's directory for an admin).
    explicit AnalysisTabW(AppSettings& settings, AnalysisManager* analysisMgr,
                          const QStringList& extraDirectories = {}, QWidget* parent = nullptr);
    ~AnalysisTabW() override;

   private:
    void build_ui();
    void rebuild_session_list();
    void select_session(const QString& path);
    void select_camera(int index);
    void select_plugin(int index);
    void on_pose_model_changed();
    void run_analysis();
    void reload_current_camera_result();
    void update_kinematics_chart();
    void export_kinematics_csv();
    void rebuild_subject_chips(int subjectCount);
    [[nodiscard]] QVector<int> checked_subject_indices() const;
    void open_output_folder();
    void update_transcript_table();
    void export_transcript_csv();
    void highlight_active_transcript_row(int64_t ms);
    void rebuild_speaker_legend();
    void update_expression_view();
    void export_expression_csv();
    void update_gaze_view();
    void export_gaze_csv();
    void update_pose3d_view();
    void export_skeleton3d_csv();
    void update_dyadic_view();
    void export_dyad_csv();
    void update_trigger_sync_view();
    void export_trigger_sync_csv();
    void update_rppg_view();
    void export_rppg_csv();
    void update_gaze2d_view();
    void export_gaze2d_csv();
    [[nodiscard]] bool is_pose_plugin() const;
    [[nodiscard]] bool is_diarize_plugin() const;
    [[nodiscard]] bool is_expression_plugin() const;
    [[nodiscard]] bool is_face_mask_plugin() const;
    [[nodiscard]] bool is_gaze_fusion_plugin() const;
    [[nodiscard]] bool is_pose3d_plugin() const;
    [[nodiscard]] bool is_trigger_sync_plugin() const;
    [[nodiscard]] bool is_rppg_plugin() const;
    [[nodiscard]] bool is_gaze2d_plugin() const;
    [[nodiscard]] bool is_pose_depth_selected() const;
    [[nodiscard]] QString slug_for_model(const QString& modelId) const;
    [[nodiscard]] QString pose_json_path_for(const QString& videoRelPath) const;
    [[nodiscard]] QString depth_video_path_for(const QString& videoRelPath) const;
    [[nodiscard]] QString anonymized_video_path_for(const QString& videoRelPath) const;
    [[nodiscard]] QString transcript_json_path_for(const QString& audioRelPath) const;
    [[nodiscard]] QString expression_json_path_for(const QString& videoRelPath) const;
    [[nodiscard]] QString rppg_json_path_for(const QString& videoRelPath) const;
    [[nodiscard]] QString gaze2d_json_path_for(const QString& videoRelPath) const;

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
