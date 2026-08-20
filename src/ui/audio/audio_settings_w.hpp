#pragma once
#include <QVBoxLayout>
#include <QWidget>
#include <memory>

#include "audio/audio_manager.hpp"
#include "core/settings.hpp"
#include "ui/audio/audio_waveform_w.hpp"

namespace mosaic {

class AudioSettingsW : public QWidget {
    Q_OBJECT
   public:
    // audioMgr may be nullptr in unit tests; level meters are simply inactive.
    explicit AudioSettingsW(AudioSettings& settings, AudioManager* audioMgr,
                            QWidget* parent = nullptr);
    ~AudioSettingsW() override;

   signals:
    void settings_changed();

   private:
    void build_encoding_section(QVBoxLayout* parent);
    void build_microphones_section(QVBoxLayout* parent);
    void build_waveform_section(QVBoxLayout* parent);
    void add_microphone(MicrophoneParameters params = {});
    void remove_microphone(int index);

    AudioSettings& m_settings;

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
