#pragma once
#include <QAudioFormat>
#include <QAudioSource>
#include <QByteArray>
#include <QObject>
#include <atomic>
#include <memory>

#include "audio/wav_writer.hpp"
#include "core/settings.hpp"

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
    explicit AudioRecorder(const MicrophoneParameters& params, QObject* parent = nullptr);
    ~AudioRecorder() override;

    // Finds the device matching params.deviceId and starts recording.
    // Falls back to the system default input if the device is not found.
    [[nodiscard]] bool start(const QString& filePath);
    void stop();

    [[nodiscard]] bool is_recording() const;
    [[nodiscard]] float level_rms() const; // 0.0–1.0, updated every buffer
    [[nodiscard]] double duration_sec() const;

   signals:
    void level_rms_changed(float rms); // 10–20 × per second, main-thread safe
    void envelope_changed(float minSample, float maxSample); // same cadence, [-1, 1]
    // Same cadence as envelope_changed — the already-16-bit-PCM-normalized
    // `data` from on_data_ready() (see m_captureFormat's doc comment),
    // before it's discarded. sampleRate/channels are whatever was actually
    // negotiated by start() (see its own doc comment — NOT guaranteed to
    // match MicrophoneParameters' requested values).
    void raw_pcm_ready(QByteArray pcm16, int sampleRate, int channels);
    void error_occurred(QString message);

   private slots:
    void on_data_ready();

   private:
    const MicrophoneParameters& m_params;
    std::unique_ptr<QAudioSource> m_source;
    QIODevice* m_ioDevice{nullptr};
    WavWriter m_writer;
    std::atomic<float> m_level{0.0f};
    bool m_monitorOnly{false}; // true when filePath is "" (no file writing)

    // Whichever sample format the device actually ended up capturing in —
    // not every device supports 16-bit PCM at all (many professional/USB
    // Audio Class 2.0 interfaces only offer 32-bit float or 32-bit int).
    // on_data_ready() converts every buffer to 16-bit PCM based on this
    // before it reaches m_writer/compute_rms()/compute_envelope(), so
    // nothing downstream needs to know the device's native format.
    QAudioFormat::SampleFormat m_captureFormat{QAudioFormat::Int16};
    int m_sampleRate{0}; // actually negotiated rate, set in start()
    int m_channels{0};   // actually negotiated channel count, set in start()
};

} // namespace mosaic
