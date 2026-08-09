#pragma once
#include "core/settings.hpp"
#include <QVBoxLayout>
#include <QWidget>
#include <memory>

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

private:
    void build_encoding_section(QVBoxLayout* parent);
    void build_cameras_section(QVBoxLayout* parent);
    void make_card(int index);          // create a card for cameras[index], no push_back
    void add_camera(CameraParameters params = {}); // push_back + make_card
    void discover_cameras();            // enumerate Pylon devices, add_camera() for new ones

    VideoSettings& m_settings;

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
