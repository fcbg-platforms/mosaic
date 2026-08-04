#pragma once
#include <QList>
#include <QPair>
#include <QString>

namespace mosaic {

/// @brief Display label / `--model` value pairs for the YOLOv8-pose variants
/// shipped with `analysis/pose/human_pose.py`. Shared between every UI
/// control that lets the user pick a pose model, so the option list only
/// exists in one place.
inline QList<QPair<QString, QString>> pose_model_options() {
    return {
        {"YOLOv8n-pose  (fastest, CPU OK)",  "yolov8n-pose.pt"},
        {"YOLOv8s-pose  (balanced)",         "yolov8s-pose.pt"},
        {"YOLOv8m-pose  (accurate, GPU)",    "yolov8m-pose.pt"},
        {"YOLOv8l-pose  (best, GPU req.)",   "yolov8l-pose.pt"},
    };
}

/// @brief Display label / `--model` value pairs for the YOLO26-depth variants
/// `analysis/pose/depth_estimator.py` supports — a monocular depth-map
/// estimator, a completely different output shape from the keypoint models
/// above (see is_depth_model()). Shown in the same model combo as the
/// keypoint options, appended after a separator.
inline QList<QPair<QString, QString>> pose_depth_model_options() {
    return {
        {"YOLO26n Depth  (fastest, experimental)",  "yolo26n-depth.pt"},
        {"YOLO26s Depth  (balanced, experimental)", "yolo26s-depth.pt"},
        {"YOLO26m Depth  (accurate, experimental)", "yolo26m-depth.pt"},
    };
}

/// @param modelId  A `--model` value from pose_model_options()/
///                 pose_depth_model_options() (e.g. a modelCombo itemData).
/// @returns  True if this is a depth-estimation model rather than a
///           pose/keypoint model — these need a completely different
///           downstream code path (a colorized depth video with no
///           keypoints/skeleton data at all) both in analysis/run_pose.py
///           and in AnalysisTabW's results view.
[[nodiscard]] inline bool is_depth_model(const QString& modelId) {
    return modelId.contains("depth");
}

} // namespace mosaic
