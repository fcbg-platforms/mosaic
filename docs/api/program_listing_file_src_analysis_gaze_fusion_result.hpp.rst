
.. _program_listing_file_src_analysis_gaze_fusion_result.hpp:

Program Listing for File gaze_fusion_result.hpp
===============================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_analysis_gaze_fusion_result.hpp>` (``src\analysis\gaze_fusion_result.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include <QRectF>
   #include <QString>
   #include <QStringList>
   #include <QVector>
   #include <array>
   #include <cstdint>
   
   namespace mosaic {
   
   /// Plain 3-vector (room-space mm) — std::array rather than QVector3D
   /// deliberately, matching CalibrationData's own std::array<double,N>
   /// convention for geometric data (src/core/settings.hpp) and keeping this
   /// class free of the QtGui dependency QVector3D would pull in.
   using Vec3 = std::array<double, 3>;
   
   /// One contributing camera's raw gaze data within a single fused frame.
   /// Mirrors run_gaze_fusion.py's per-frame "per_camera" entries exactly.
   struct GazeFusionCamera {
       int    cameraIndex = -1;
       QRectF faceBoxPx;              ///< face_box_px, that camera's video pixel space.
       double gazeDx = 0.0, gazeDy = 0.0;
       Vec3   originRoom    = {0, 0, 0};
       Vec3   directionRoom = {0, 0, 0};
       double confidence    = 0.0;
   };
   
   /// Static per-camera room position (extrinsic_rt's translation column),
   /// written once per file — used by the room-view widget's camera icons.
   struct GazeFusionRoomCamera {
       int  index = -1;
       Vec3 positionRoom = {0, 0, 0};
   };
   
   /// One fused master-tick. Mirrors run_gaze_fusion.py's per-frame JSON object.
   struct GazeFusionFrame {
       int64_t tick           = 0;
       int64_t timestampNs    = 0;
       int     numCameras     = 0;
       bool    isTriangulated = false;
       Vec3    fusedOriginRoom    = {0, 0, 0};
       Vec3    fusedDirectionRoom = {0, 0, 0};
       double  residualRmsMm  = -1.0;   ///< -1 = not applicable (numCameras < 2).
       bool    hasTarget      = false;
       Vec3    targetPointRoom = {0, 0, 0};
       QVector<GazeFusionCamera> perCamera;
   };
   
   /// Parses a session-root "gaze_fusion.json" file written by
   /// analysis/run_gaze_fusion.py into a queryable in-memory structure, for the
   /// Analysis tab's Multi-Camera Gaze Fusion plugin (per-camera bbox+arrow
   /// overlay during playback, plus a 3D room-view widget). Mirrors
   /// ExpressionResult (src/analysis/expression_result.hpp) closely — same
   /// load()/is_valid() shape — with one deliberate divergence: frames are
   /// keyed on the shared master tick/timestamp (not any single video's
   /// frame_index, since fusion is inherently cross-camera), so nearest-frame
   /// lookup is by timestamp, not frame index.
   ///
   /// Usage:
   /// @code
   ///   auto result = GazeFusionResult::load(jsonPath);
   ///   if (result.is_valid()) { ... }
   /// @endcode
   class GazeFusionResult {
   public:
       GazeFusionResult() = default;
   
       /// Parses jsonPath. Returns a default-constructed (is_valid() == false)
       /// result if the file is missing or malformed.
       static GazeFusionResult load(const QString& jsonPath);
   
       [[nodiscard]] bool is_valid() const { return valid_; }
       [[nodiscard]] const QStringList& source_videos() const { return sourceVideos_; }
       [[nodiscard]] const QVector<GazeFusionRoomCamera>& cameras() const { return cameras_; }
       [[nodiscard]] bool plane_defined() const { return planeDefined_; }
       [[nodiscard]] Vec3 plane_point() const { return planePoint_; }
       [[nodiscard]] Vec3 plane_normal() const { return planeNormal_; }
       [[nodiscard]] double master_fps() const { return masterFps_; }
       [[nodiscard]] const QVector<GazeFusionFrame>& frames() const { return frames_; }
   
       /// Nearest-frame lookup by timestamp (ns) estimate — binary search,
       /// since frames() is stored in the ascending timestampNs order
       /// run_gaze_fusion.py writes ascending master ticks in. Returns
       /// nullptr if there are no frames.
       [[nodiscard]] const GazeFusionFrame* nearest_frame(int64_t timestampNsEstimate) const;
   
   private:
       bool        valid_        = false;
       QStringList sourceVideos_;
       QVector<GazeFusionRoomCamera> cameras_;
       bool        planeDefined_ = false;
       Vec3        planePoint_  = {0, 0, 0};
       Vec3        planeNormal_ = {0, 0, 1};
       double      masterFps_    = 25.0;
       QVector<GazeFusionFrame> frames_;
   };
   
   } // namespace mosaic
