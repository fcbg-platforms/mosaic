#pragma once
#include <QImage>
#include <QVBoxLayout>
#include <QWidget>
#include <memory>

#include "calibration/room_calibration_manager.hpp"
#include "core/settings.hpp"

namespace mosaic {

class VideoManager;

// Nested page (inside the restructured CalibrationW) for multi-camera
// extrinsic ("room") calibration via a shared ChArUco board. Requires every
// camera to already have a valid intrinsic calibration (Intrinsics page)
// before its shots can be used — refresh_intrinsics() re-syncs from
// videoSettings whenever that page saves a new one.
//
// Layout:
//   ┌─ ChArUco board ─────────────────────────────┐
//   │  Cols [7]  Rows [5]  Square [40] Marker [30] mm │
//   └──────────────────────────────────────────────┘
//   ┌─ Cameras (live thumbnails, aiming only) ─────┐
//   │  [cam0 ●] [cam1 ●] [cam2 ●] ...               │
//   └──────────────────────────────────────────────┘
//   ┌─ Capture ────────────────────────────────────┐
//   │  [▶ Capture shot]  Shots: 0                   │
//   │  [per-camera resolved / RMS table]            │
//   │  Reference camera [combo]  [▶ Solve]          │
//   │  [Use last shot as plane]  [Save to settings] │
//   └──────────────────────────────────────────────┘
class RoomCalibrationW : public QWidget {
    Q_OBJECT
   public:
    explicit RoomCalibrationW(VideoSettings& videoSettings, RoomSettings& roomSettings,
                              VideoManager* videoMgr, QWidget* parent = nullptr);
    ~RoomCalibrationW() override;

    // Re-reads every camera's current intrinsic calibration from
    // videoSettings into the underlying RoomCalibrationManager. Call after
    // the Intrinsics page saves a (re-)calibration.
    void refresh_intrinsics();

   signals:
    void extrinsics_saved();

   private:
    void build_board_section(QVBoxLayout* parent);
    void build_camera_section(QVBoxLayout* parent);
    void build_capture_section(QVBoxLayout* parent);

    void on_calibration_frame_ready(int cameraIndex, QImage frame, uint64_t token);
    void on_preview_frame(int cameraIndex, QImage frame);
    void finalize_shot();
    void capture_shot();
    void solve();
    void use_shot_as_plane();
    void save_to_settings();
    void update_result_table();
    void rebuild_board_spec();

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
