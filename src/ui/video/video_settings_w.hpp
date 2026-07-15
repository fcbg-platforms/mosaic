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
    void remove_camera(int index);
    void discover_cameras();            // enumerate Pylon devices, add_camera() for new ones

    VideoSettings& m_settings;

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
