#pragma once
#include "core/settings.hpp"
#include <QLabel>
#include <QProgressBar>
#include <QWidget>
#include <memory>

namespace mosaic {

// Collapsible card for one microphone device.
// Shows a real device selector (populated from QMediaDevices),
// a level meter that is updated during recording, and per-device settings.

class MicrophoneCardW : public QWidget {
    Q_OBJECT
public:
    explicit MicrophoneCardW(MicrophoneParameters& params, int index,
                              QWidget* parent = nullptr);
    ~MicrophoneCardW() override;

    // Drive the VU meter from AudioManager::level_rms_changed (0.0–1.0).
    void set_level(float rms);

    void set_connected(bool connected);
    void set_index(int index);

signals:
    void params_changed();
    void remove_requested(int index);

private:
    void build_header();
    void build_body();
    void toggle_expanded();

    MicrophoneParameters& m_params;
    int   m_index;
    bool  m_expanded{true};

    QWidget*      m_body{nullptr};
    QWidget*      m_statusDot{nullptr};
    QWidget*      m_expandBtn{nullptr};
    QLabel*       m_nameLabel{nullptr};
    QProgressBar* m_levelBar{nullptr};
};

} // namespace mosaic
