#pragma once
#include <QVBoxLayout>
#include <QWidget>
#include <memory>

#include "calibration/calibration_manager.hpp"
#include "core/settings.hpp"

namespace mosaic {

class VideoManager;

// Settings tab for camera calibration — an inner QTabWidget with two pages:
//
//   "Intrinsics" — single-camera checkerboard calibration (unchanged from
//   before this class hosted a 2nd page):
//     ┌─ Board configuration ──────────────────────┐
//     │  Cols [9]  Rows [6]  Square size [25.0] mm │
//     └────────────────────────────────────────────┘
//     ┌─ Capture ──────────────────────────────────┐
//     │  Camera [combo]  [▶ Capture frame]          │
//     │  Views accepted: 0       [Clear]            │
//     └────────────────────────────────────────────┘
//     ┌─ Preview ──────────────────────────────────┐
//     │  (last captured frame with corner overlay)  │
//     └────────────────────────────────────────────┘
//     ┌─ Result ───────────────────────────────────┐
//     │  [▶ Calibrate]  RMS: —                     │
//     │  fx: —  fy: —  cx: —  cy: —               │
//     │  [Save to settings]                        │
//     └────────────────────────────────────────────┘
//
//   "Room (Extrinsics)" — RoomCalibrationW, multi-camera simultaneous
//   ChArUco capture; see room_calibration_w.hpp.
class CalibrationW : public QWidget {
    Q_OBJECT
   public:
    // videoSettings is used to know how many cameras are configured.
    // roomSettings/videoMgr are forwarded to the Room (Extrinsics) page —
    // that page needs live multi-camera frames (videoMgr) and somewhere to
    // persist the room's reference plane (roomSettings), neither of which
    // the existing single-camera Intrinsics flow required.
    explicit CalibrationW(VideoSettings& videoSettings, RoomSettings& roomSettings,
                          VideoManager* videoMgr, QWidget* parent = nullptr);
    ~CalibrationW() override;

   signals:
    void calibration_saved(int cameraIndex);

   private slots:
    void on_corners_detected(int viewIndex, bool found, QImage preview);
    void on_calibration_done(double rmsError, bool success);

   private:
    void build_board_section(QVBoxLayout* parent);
    void build_capture_section(QVBoxLayout* parent);
    void build_preview_section(QVBoxLayout* parent);
    void build_result_section(QVBoxLayout* parent);

    void update_result_labels();
    void save_to_settings();

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
