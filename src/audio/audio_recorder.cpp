#include "audio/audio_recorder.hpp"

#include <QAudioDevice>
#include <QMediaDevices>

#include "audio/audio_envelope.hpp"
#include "utils/logger.hpp"

namespace mosaic {

// ── Device lookup ──────────────────────────────────────────────────────────

static QAudioDevice find_device(const QString& deviceId) {
    if (!deviceId.isEmpty()) {
        const QByteArray target = deviceId.toLatin1();
        for (const QAudioDevice& dev : QMediaDevices::audioInputs()) {
            if (dev.id() == target) return dev;
        }
        log_warning(
            QString("[AudioRecorder] Device '%1' not found — using default.").arg(deviceId));
    }
    return QMediaDevices::defaultAudioInput();
}

// ── AudioRecorder ──────────────────────────────────────────────────────────

AudioRecorder::AudioRecorder(const MicrophoneParameters& params, QObject* parent)
    : QObject(parent), m_params(params) {}

AudioRecorder::~AudioRecorder() { stop(); }

bool AudioRecorder::start(const QString& filePath) {
    m_monitorOnly = filePath.isEmpty();

    // 1. Find device and resolve the actual capture format FIRST — pure
    //    queries (isFormatSupported()/preferredFormat()), no hardware
    //    stream opened yet. Device fallback below can change
    //    sampleRate/channelCount, not just bit depth (see
    //    m_captureFormat's doc comment) — the WAV file is opened further
    //    down using the RESOLVED fmt, not m_params, specifically so its
    //    header can never disagree with what's actually captured. It used
    //    to be opened here, before format negotiation, using the
    //    originally-requested m_params.sampleRate/channels — a device
    //    falling back to a different rate/channel count (e.g. 48000 Hz
    //    mono when 44100 Hz stereo was requested) still wrote a header
    //    claiming the original values, producing a .wav that plays at the
    //    wrong speed/pitch with channels misinterpreted, even though
    //    on_data_ready() already correctly bit-depth-converts every
    //    sample to match the header's declared 16-bit PCM.
    const QAudioDevice device = find_device(m_params.deviceId);
    if (device.isNull()) {
        const QString msg = "No audio input device available.";
        log_error(msg);
        emit error_occurred(msg);
        return false;
    }

    QAudioFormat fmt;
    fmt.setSampleRate(m_params.sampleRate);
    fmt.setChannelCount(m_params.channels);
    fmt.setSampleFormat(QAudioFormat::Int16);

    // Fall back if the exact requested format isn't supported.
    // compute_rms()/compute_envelope() (audio_envelope.hpp) and WavWriter
    // (opened with a hardcoded 16-bit depth below) both need 16-bit signed
    // PCM — but some devices (many professional/USB Audio Class 2.0
    // interfaces) don't support 16-bit PCM capture AT ALL, only 32-bit
    // float or 32-bit int. Rather than either (a) silently accepting
    // device.preferredFormat() wholesale, which could change the sample
    // format without saying so and corrupt every downstream byte
    // interpretation, or (b) refusing to support that whole class of
    // hardware, capture in whichever native format the device actually
    // offers and convert every buffer to 16-bit PCM ourselves in
    // on_data_ready() (see m_captureFormat).
    if (!device.isFormatSupported(fmt)) {
        QAudioFormat preferred    = device.preferredFormat();
        QAudioFormat int16Attempt = preferred;
        int16Attempt.setSampleFormat(QAudioFormat::Int16);
        if (device.isFormatSupported(int16Attempt)) {
            fmt = int16Attempt;
            log_warning(QString("[AudioRecorder] Requested format not supported by '%1'. "
                                "Falling back to %2 Hz, %3 ch (16-bit PCM).")
                            .arg(device.description())
                            .arg(fmt.sampleRate())
                            .arg(fmt.channelCount()));
        } else if (preferred.sampleFormat() == QAudioFormat::Float ||
                   preferred.sampleFormat() == QAudioFormat::Int32) {
            fmt = preferred;
            log_warning(QString("[AudioRecorder] '%1' has no 16-bit PCM mode — capturing "
                                "natively at %2 Hz, %3 ch (%4) and converting to 16-bit PCM.")
                            .arg(device.description())
                            .arg(fmt.sampleRate())
                            .arg(fmt.channelCount())
                            .arg(fmt.sampleFormat() == QAudioFormat::Float ? "32-bit float"
                                                                           : "32-bit int"));
        } else {
            const QString msg = QString(
                                    "Device '%1' offers no 16-bit, 32-bit float, or "
                                    "32-bit int capture format Mosaic can convert.")
                                    .arg(device.description());
            log_error(msg);
            emit error_occurred(msg);
            return false;
        }
    }
    m_captureFormat = fmt.sampleFormat();
    m_sampleRate    = fmt.sampleRate();
    m_channels      = fmt.channelCount();

    // 2. Open WAV file now that the real capture format is known — still
    //    fails early before touching hardware (QAudioSource isn't created
    //    until step 3). In monitor-only mode (empty filePath), skip file
    //    creation. Always 16-bit PCM on disk regardless of the device's
    //    native capture format — on_data_ready() converts every buffer to
    //    match before writing.
    if (!m_monitorOnly) {
        if (!m_writer.open(filePath, fmt.sampleRate(), fmt.channelCount(), 16)) {
            const QString msg = QString("Cannot create WAV file: %1").arg(filePath);
            log_error(msg);
            emit error_occurred(msg);
            return false;
        }
    }

    // 3. Create and start QAudioSource.
    m_source = std::make_unique<QAudioSource>(device, fmt, this);
    m_source->setBufferSize(fmt.sampleRate() * fmt.channelCount() * fmt.bytesPerSample() /
                            10); // ~100 ms buffer

    m_ioDevice = m_source->start(); // pull mode
    if (!m_ioDevice || m_source->error() != QAudio::NoError) {
        const QString msg = QString("Failed to open audio input '%1'.").arg(device.description());
        log_error(msg);
        emit error_occurred(msg);
        if (!m_monitorOnly) m_writer.close();
        m_source.reset();
        return false;
    }

    connect(m_ioDevice, &QIODevice::readyRead, this, &AudioRecorder::on_data_ready);

    log_info(QString("[AudioRecorder] Started: '%1' → %2")
                 .arg(device.description(), m_monitorOnly ? "(monitor only)" : filePath));
    return true;
}

void AudioRecorder::stop() {
    if (m_source) {
        m_source->stop();
        m_source.reset();
    }
    m_ioDevice = nullptr;
    m_writer.close();
    m_level.store(0.0f, std::memory_order_relaxed);
}

// ── Audio data ─────────────────────────────────────────────────────────────

void AudioRecorder::on_data_ready() {
    if (!m_ioDevice) return;
    QByteArray data = m_ioDevice->readAll();
    if (data.isEmpty()) return;

    // Normalize to 16-bit PCM here, once, so WavWriter/compute_rms()/
    // compute_envelope() below never need to know the device's native
    // capture format (see m_captureFormat's doc comment).
    if (m_captureFormat == QAudioFormat::Float) {
        data = convert_float32_to_int16(data.constData(), data.size());
    } else if (m_captureFormat == QAudioFormat::Int32) {
        data = convert_int32_to_int16(data.constData(), data.size());
    }

    if (!m_monitorOnly) m_writer.write(data.constData(), data.size());

    emit raw_pcm_ready(data, m_sampleRate, m_channels);

    const float rms = compute_rms(data.constData(), data.size());
    m_level.store(rms, std::memory_order_relaxed);
    emit level_rms_changed(rms);

    const AudioEnvelope env = compute_envelope(data.constData(), data.size());
    emit envelope_changed(env.minSample, env.maxSample);
}

// ── Accessors ──────────────────────────────────────────────────────────────

bool AudioRecorder::is_recording() const {
    return m_monitorOnly ? (m_source != nullptr) : m_writer.is_open();
}
float AudioRecorder::level_rms() const { return m_level.load(std::memory_order_relaxed); }
double AudioRecorder::duration_sec() const { return m_writer.duration_sec(); }

} // namespace mosaic
