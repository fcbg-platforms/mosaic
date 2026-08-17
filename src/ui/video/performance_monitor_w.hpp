#pragma once
#include <QWidget>
#include <memory>

#include "analysis/analysis_manager.hpp"
#include "audio/audio_manager.hpp"
#include "video/video_manager.hpp"

namespace mosaic {

// Real-time performance dashboard shown in the "Perf" sidebar tab.
//
// Polls VideoManager::camera_stats() every second via a QTimer and renders:
//   • Per-camera row: FPS, frames grabbed/encoded, dropped frames, ring-fill bar
//   • Summary row: totals and recording state
//   • Audio row: mic count, active recordings
//
// All updates happen on the main thread (QTimer fires there).

class PerformanceMonitorW : public QWidget {
    Q_OBJECT
   public:
    explicit PerformanceMonitorW(VideoManager* videoMgr, AudioManager* audioMgr,
                                 AnalysisManager* analysisMgr = nullptr, QWidget* parent = nullptr);
    ~PerformanceMonitorW() override;

   private slots:
    void on_tick();

   private:
    void build_ui();
    void rebuild_camera_rows();

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
