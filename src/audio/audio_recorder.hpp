#pragma once
#include "audio/wav_writer.hpp"
#include "core/settings.hpp"
#include <QAudioFormat>
#include <QAudioSource>
#include <QObject>
#include <atomic>
#include <memory>

namespace mosaic {

// Records one microphone device to a WAV file.
// One AudioRecorder is created per MicrophoneParameters entry.
//
// Level metering: emits level_rms in the range [0.0, 1.0] after every
// incoming audio buffer so callers can drive a VU meter, and envelope_changed
// with a signed min/max pair (see audio_envelope.hpp) so callers can drive a
// real bipolar waveform display.

class AudioRecorder : public QObject {
    Q_OBJECT
public:
    explicit AudioRecorder(const MicrophoneParameters& params,
                            QObject* parent = nullptr);
    ~AudioRecorder() override;

    // Finds the device matching params.deviceId and starts recording.
    // Falls back to the system default input if the device is not found.
    [[nodiscard]] bool start(const QString& filePath);
    void               stop();

    [[nodiscard]] bool   is_recording()  const;
    [[nodiscard]] float  level_rms()     const;   // 0.0–1.0, updated every buffer
    [[nodiscard]] double duration_sec()  const;

signals:
    void level_rms_changed(float rms);       // 10–20 × per second, main-thread safe
    void envelope_changed(float minSample, float maxSample);  // same cadence, [-1, 1]
    void error_occurred(QString message);

private slots:
    void on_data_ready();

private:
    const MicrophoneParameters& m_params;
    std::unique_ptr<QAudioSource> m_source;
    QIODevice*   m_ioDevice{nullptr};
    WavWriter    m_writer;
    std::atomic<float> m_level{0.0f};
    bool         m_monitorOnly{false};   // true when filePath is "" (no file writing)

    // Whichever sample format the device actually ended up capturing in —
    // not every device supports 16-bit PCM at all (many professional/USB
    // Audio Class 2.0 interfaces only offer 32-bit float or 32-bit int).
    // on_data_ready() converts every buffer to 16-bit PCM based on this
    // before it reaches m_writer/compute_rms()/compute_envelope(), so
    // nothing downstream needs to know the device's native format.
    QAudioFormat::SampleFormat m_captureFormat{QAudioFormat::Int16};
};

} // namespace mosaic
