#pragma once
#include "calibration/calibration_manager.hpp"
#include "core/settings.hpp"
#include <QVBoxLayout>
#include <QWidget>
#include <memory>

namespace mosaic {

// Settings tab for camera checkerboard calibration.
//
// Layout:
//   ┌─ Board configuration ──────────────────────┐
//   │  Cols [9]  Rows [6]  Square size [25.0] mm │
//   └────────────────────────────────────────────┘
//   ┌─ Capture ──────────────────────────────────┐
//   │  Camera [combo]  [▶ Capture frame]          │
//   │  Views accepted: 0       [Clear]            │
//   └────────────────────────────────────────────┘
//   ┌─ Preview ──────────────────────────────────┐
//   │  (last captured frame with corner overlay)  │
//   └────────────────────────────────────────────┘
//   ┌─ Result ───────────────────────────────────┐
//   │  [▶ Calibrate]  RMS: —                     │
//   │  fx: —  fy: —  cx: —  cy: —               │
//   │  [Save to settings]                        │
//   └────────────────────────────────────────────┘

class CalibrationW : public QWidget {
    Q_OBJECT
public:
    // videoSettings is used to know how many cameras are configured.
    explicit CalibrationW(VideoSettings& videoSettings, QWidget* parent = nullptr);
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
