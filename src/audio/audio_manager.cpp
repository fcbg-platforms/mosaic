#include "audio/audio_manager.hpp"
#include "audio/audio_recorder.hpp"
#include "utils/logger.hpp"
#include <QMediaDevices>

namespace mosaic {

struct AudioManager::Impl {
    std::vector<std::unique_ptr<AudioRecorder>> recorders;
    std::vector<std::unique_ptr<AudioRecorder>> monitorRecorders;
    bool recording{false};
};

AudioManager::AudioManager(QObject* parent)
    : QObject(parent), d(std::make_unique<Impl>()) {}

AudioManager::~AudioManager() { stop(); }

// ── Device enumeration ─────────────────────────────────────────────────────

QList<QAudioDevice> AudioManager::available_inputs() {
    return QMediaDevices::audioInputs();
}

QAudioDevice AudioManager::default_input() {
    return QMediaDevices::defaultAudioInput();
}

// ── Recording lifecycle ────────────────────────────────────────────────────

void AudioManager::start_monitoring(const std::vector<MicrophoneParameters>& microphones) {
    stop_monitoring();
    if (microphones.empty()) return;

    log_info(QString("[AudioManager] Starting %1 monitor-only recorder(s).").arg(microphones.size()));
    d->monitorRecorders.reserve(microphones.size());

    for (int i = 0; i < static_cast<int>(microphones.size()); ++i) {
        auto rec = std::make_unique<AudioRecorder>(microphones[static_cast<size_t>(i)], this);
        connect(rec.get(), &AudioRecorder::level_rms_changed, this,
                [this, i](float rms) { emit level_rms_changed(i, rms); },
                Qt::QueuedConnection);
        if (!rec->start("")) {  // monitor-only mode
            log_warning(QString("[AudioManager] Monitor recorder %1 failed to start.").arg(i));
        }
        d->monitorRecorders.push_back(std::move(rec));
    }
}

void AudioManager::stop_monitoring() {
    for (auto& rec : d->monitorRecorders) rec->stop();
    d->monitorRecorders.clear();
}

void AudioManager::start(const QString& sessionDir,
                          const QString& basename,
                          const std::vector<MicrophoneParameters>& microphones) {
    stop_monitoring();  // monitoring and recording are mutually exclusive
    stop();             // clean up any previous recording session

    if (microphones.empty()) {
        log_info("[AudioManager] No microphones configured — skipping audio recording.");
        return;
    }

    log_info(QString("[AudioManager] Starting %1 recorder(s).").arg(microphones.size()));
    d->recorders.reserve(microphones.size());

    for (int i = 0; i < static_cast<int>(microphones.size()); ++i) {
        const auto& mic = microphones[static_cast<size_t>(i)];

        // Build file path: sessionDir/basename_0.wav, _1.wav, …
        const QString filePath = sessionDir + "/" + basename
            + (microphones.size() > 1 ? "_" + QString::number(i) : "")
            + ".wav";

        auto rec = std::make_unique<AudioRecorder>(mic, this);

        // Forward level and error signals with the mic index.
        connect(rec.get(), &AudioRecorder::level_rms_changed, this,
                [this, i](float rms) { emit level_rms_changed(i, rms); },
                Qt::QueuedConnection);

        connect(rec.get(), &AudioRecorder::error_occurred, this,
                [this, i](const QString& msg) { emit recorder_error(i, msg); },
                Qt::QueuedConnection);

        if (!rec->start(filePath)) {
            log_error(QString("[AudioManager] Recorder %1 failed to start.").arg(i));
        }

        d->recorders.push_back(std::move(rec));
    }

    d->recording = true;
}

void AudioManager::stop() {
    for (auto& rec : d->recorders) rec->stop();
    d->recorders.clear();
    d->recording = false;
}

bool AudioManager::is_monitoring() const { return !d->monitorRecorders.empty(); }
bool AudioManager::is_recording()  const { return d->recording; }
int  AudioManager::recorder_count() const {
    return static_cast<int>(d->recorders.size());
}

} // namespace mosaic
