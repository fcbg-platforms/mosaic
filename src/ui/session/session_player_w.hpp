#pragma once
#include "analysis/sync_manifest.hpp"
#include "session/session_info.hpp"
#include <QDialog>
#include <memory>

namespace mosaic {

// ── SessionPlayerW ────────────────────────────────────────────────────────
//
// Synchronised multi-camera session player.
//
// Opens all video_N.mp4 files in the session folder and plays them aligned
// to a common wall-clock timeline defined by the SyncManifest.  If no
// sync_manifest.json exists it is generated automatically before playback.
//
// Features:
//   • 6-camera grid with per-slot fps / coverage badges
//   • Synchronised audio playback (audio.wav)
//   • Scrubber with millisecond resolution
//   • Speed control (0.25×, 0.5×, 1×, 2×, 4×)
//   • Frame stepping (◀ / ▶)
//   • Annotation timeline bar with colour-coded blocks
//   • Automatic drift correction every 500 ms
//   • Keyboard shortcuts: Space play/pause, ← → seek 2 s, [ ] speed

class SessionPlayerW : public QDialog {
    Q_OBJECT
public:
    explicit SessionPlayerW(const SessionInfo& session,
                             QWidget*           parent = nullptr);
    ~SessionPlayerW() override;

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void on_play_pause();
    void on_stop();
    void on_step(int direction);      // direction: -1 or +1
    void on_scrub_moved(int valueMs);
    void on_speed_changed(int index);
    void on_volume_changed(int value);
    void on_sync_tick();
    void on_master_timer_tick();

private:
    void build_ui();
    void setup_players();
    void seek_all(int64_t masterMs);
    void apply_playback_state();
    void update_time_display(int64_t masterMs);
    void update_camera_overlays();

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
