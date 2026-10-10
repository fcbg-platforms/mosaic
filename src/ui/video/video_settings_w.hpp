#pragma once
#include <QVBoxLayout>
#include <QWidget>
#include <memory>

#include "core/settings.hpp"

namespace mosaic {

// The complete video settings panel: global encoding options at the top,
// then a scrollable list of per-camera collapsible cards below.
//
// The widget writes changes directly into the VideoSettings reference it holds
// and emits settings_changed() after each user action so the parent knows to
// schedule a settings save.

class VideoSettingsW : public QWidget {
    Q_OBJECT
   public:
    explicit VideoSettingsW(VideoSettings& settings, QWidget* parent = nullptr);
    ~VideoSettingsW() override;

    // Enables/disables the "Discover cameras" button. Used by MainWindow to
    // keep it disabled whenever a camera session (preview or recording) is
    // active — VideoGrabber::enumerate_devices() and a live
    // ActionCommandTicker both touch Pylon's CTlFactory, and the SDK
    // documents no thread-safety guarantee for concurrent use, so this
    // closes off the one UI path that could race it.
    void set_discover_enabled(bool enabled);

    // Passthrough to the matching card's CameraCardW::set_action_command_capability()
    // — cameraIndex is the camera's position in VideoSettings::cameras (same
    // convention as camera_params_changed's index). No-op if out of range.
    void set_action_command_capability(int cameraIndex, bool supported);

    // Passthrough to the matching card's CameraCardW::set_achievable_fps() —
    // same config-index convention as above. `fps` <= 0 means "no trustworthy
    // measurement", which the card renders as its awaiting-measurement or
    // exposure-ceiling state rather than as a number. No-op if out of range.
    void set_achievable_fps(int cameraIndex, double fps);

    // Re-reads the interview section from VideoSettings::interview. Called by
    // MainWindow after it switched the mode — from here or from the toggle
    // above the live feeds — so both controls always show what is in effect,
    // and after a refused switch, to put the checkbox back.
    void sync_interview_from_settings();

    // Locks the interview section while a recording runs. Switching mode or
    // applying a new crop reopens the cameras, which would end the recording;
    // MainWindow refuses that anyway, and a control that visibly cannot be
    // used says so earlier than a refusal in the log.
    void set_recording_locked(bool locked);

    // The interview camera's own reported maximum frame rate for the crop it
    // was last opened with (-1 = unknown / not open). Shown at once, before
    // any measurement, with a "Use N fps" button when the rate asked for is
    // more than the camera can do.
    void set_interview_camera_max_fps(double fps);

   signals:
    void settings_changed();
    // Fired only when cameras are added or removed (not on per-camera param changes).
    // Connect this to trigger a VideoManager hardware reload.
    void cameras_list_changed();
    // Fired whenever one camera's own parameters change (in addition to the
    // bare settings_changed() above). `index` is the camera's position in
    // VideoSettings::cameras — connect this to push the edit live to an
    // already-open camera (VideoManager::apply_live_params) instead of
    // waiting for the next full reopen.
    void camera_params_changed(int index);

    // The operator asked to turn interview mode on or off. Not applied here:
    // MainWindow owns the reopen and may refuse (a recording is running), and
    // answers by calling sync_interview_from_settings() either way. Any
    // staged interview edits have already been written to the settings when
    // this fires with `on`, so the mode opens with what is on screen.
    void interview_mode_requested(bool on);
    // Staged interview edits were written to VideoSettings::interview by
    // "Apply". MainWindow reopens the cameras if interview mode is open; if it
    // is off there is nothing to reopen, and the values wait for the next
    // switch.
    void interview_settings_applied();

   private:
    void build_encoding_section(QVBoxLayout* parent);
    void build_sync_section(QVBoxLayout* parent);
    void build_cameras_section(QVBoxLayout* parent);
    void build_interview_section(QVBoxLayout* parent);
    // Interview section helpers — see the .cpp.
    void rebuild_interview_camera_combo(int select);
    void load_interview_fields(const InterviewSettings& s);
    [[nodiscard]] InterviewSettings staged_interview() const;
    void refresh_interview_readouts();
    void make_card(int index);                     // create a card for cameras[index], no push_back
    void add_camera(CameraParameters params = {}); // push_back + make_card
    void discover_cameras(); // enumerate Pylon devices, add_camera() for new ones

    VideoSettings& m_settings;

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
