
.. _program_listing_file_src_analysis_pose_analysis_result.hpp:

Program Listing for File pose_analysis_result.hpp
=================================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_analysis_pose_analysis_result.hpp>` (``src\analysis\pose_analysis_result.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include <QPair>
   #include <QPointF>
   #include <QRectF>
   #include <QString>
   #include <QStringList>
   #include <QVector>
   #include <cstdint>
   
   namespace mosaic {
   
   /// One detected subject's keypoints within a single analysed frame.
   /// Mirrors run_pose.py's _result_to_dict() "subjects" entries exactly.
   struct PoseSubject {
       int               subjectId  = -1;
       double            confidence = 0.0;
       QVector<QPointF>  keypoints;      ///< Pixel coordinates, one per keypoint name.
       QVector<double>   visibilities;   ///< 0-1, same order as keypoints.
       QRectF            bbox;           ///< bbox_xyxy.
   };
   
   /// Shared visibility threshold for "trust this keypoint" decisions — used
   /// both to decide whether to draw a keypoint (SkeletonOverlayW::paintEvent,
   /// src/ui/analysis/pose_overlay_player_w.cpp) and whether to include it in
   /// derived kinematics (pose_kinematics.cpp). Kept in one place so the two
   /// can't silently drift to different thresholds. A missing visibilities
   /// entry (index >= visibilities.size()) defaults to "visible" — matches
   /// keypoints[] always being written 1:1 with visibilities[] by run_pose.py,
   /// so this only matters for a malformed/truncated file, where treating an
   /// unknown entry as visible is the same permissive default this codebase
   /// already used before this helper existed.
   inline bool is_keypoint_visible(const PoseSubject& subject, int keypointIndex) {
       return subject.visibilities.value(keypointIndex, 1.0) >= 0.1;
   }
   
   /// One analysed frame. Mirrors run_pose.py's per-frame JSON object.
   struct PoseFrame {
       int                 frameIndex  = 0;
       int64_t             timestampNs = 0;
       int                 cameraIndex = 0;
       QVector<PoseSubject> subjects;
   };
   
   /// Parses a .pose.json file written by analysis/run_pose.py (--out-format json)
   /// into a queryable in-memory structure, for drawing a live overlay during
   /// playback and plotting per-keypoint metrics over time.
   ///
   /// Usage:
   /// @code
   ///   auto result = PoseAnalysisResult::load(jsonPath);
   ///   if (result.is_valid()) { ... }
   /// @endcode
   class PoseAnalysisResult {
   public:
       PoseAnalysisResult() = default;
   
       /// Parses jsonPath. Returns a default-constructed (is_valid() == false)
       /// result if the file is missing or malformed.
       static PoseAnalysisResult load(const QString& jsonPath);
   
       [[nodiscard]] bool is_valid() const { return valid_; }
   
       /// True if at least one frame has at least one detected subject.
       /// Distinct from is_valid() (which only means "the JSON parsed"): a
       /// result can be valid but empty if the pose model detected nobody in
       /// this camera's footage for the whole session — callers use this to
       /// tell that case apart from "hasn't been analyzed yet".
       [[nodiscard]] bool has_any_detections() const {
           for (const auto& f : frames_) { if (!f.subjects.isEmpty()) { return true; } }
           return false;
       }
   
       [[nodiscard]] const QString& source_video() const { return sourceVideo_; }
       /// The model id that produced this result (e.g. "yolov8n-pose.pt"), from
       /// the JSON's top-level "model" field — empty for files written before
       /// this field existed (older, un-namespaced .pose.json files still load
       /// fine, they just report no model).
       [[nodiscard]] const QString& model() const { return model_; }
       [[nodiscard]] const QStringList& keypoint_names() const { return keypointNames_; }
       [[nodiscard]] const QVector<QPair<int, int>>& skeleton_edges() const { return skeletonEdges_; }
       [[nodiscard]] const QVector<PoseFrame>& frames() const { return frames_; }
   
       /// Nearest-frame lookup by frame_index estimate (e.g. derived from a
       /// video playback position). frames() is stored in the ascending
       /// frame_index order run_pose.py writes them in, so this is a binary
       /// search, not a linear scan. Returns nullptr if there are no frames.
       [[nodiscard]] const PoseFrame* nearest_frame(int frameIndexEstimate) const;
   
   private:
       bool                     valid_ = false;
       QString                  sourceVideo_;
       QString                  model_;
       QStringList              keypointNames_;
       QVector<QPair<int, int>> skeletonEdges_;
       QVector<PoseFrame>       frames_;
   };
   
   } // namespace mosaic
