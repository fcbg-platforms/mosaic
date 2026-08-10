#include "analysis/transcript_worker.hpp"
#include "utils/logger.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <cstring>

namespace mosaic {

struct TranscriptWorker::Impl {
    std::unique_ptr<QProcess> proc;
    QByteArray                readBuf;
    bool                      paused{false};
};

TranscriptWorker::TranscriptWorker(QObject* parent)
    : QObject(parent), d(std::make_unique<Impl>()) {}

TranscriptWorker::~TranscriptWorker() { stop(); }

bool TranscriptWorker::start(const QString& interpreter, const QString& scriptPath,
                              const QStringList& extraArgs) {
    stop();

    d->proc = std::make_unique<QProcess>(this);
    d->proc->setProgram(interpreter);
    d->proc->setArguments(QStringList{"-u", scriptPath} + extraArgs);

    connect(d->proc.get(), &QProcess::readyReadStandardOutput, this, [this] {
        d->readBuf += d->proc->readAllStandardOutput();
        while (true) {
            const int nl = d->readBuf.indexOf('\n');
            if (nl < 0) break;
            const QByteArray line = d->readBuf.left(nl);
            d->readBuf.remove(0, nl + 1);

            const auto doc = QJsonDocument::fromJson(line);
            if (!doc.isObject()) continue;
            const auto obj = doc.object();
            const int mic = obj["mic"].toInt();

            for (const auto& segVal : obj["new_final_segments"].toArray()) {
                const auto seg = segVal.toObject();
                emit transcript_final(mic, seg["text"].toString());
            }
            emit transcript_partial(mic, obj["tentative_text"].toString());
        }
    });

    connect(d->proc.get(), &QProcess::readyReadStandardError, this, [this] {
        const QString msg = QString::fromLocal8Bit(d->proc->readAllStandardError()).trimmed();
        if (!msg.isEmpty()) log_warning("[TranscriptWorker] " + msg);
    });

    connect(d->proc.get(), &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError err) {
        const QString msg = QString("Transcript process error: %1").arg(static_cast<int>(err));
        log_error(msg);
        emit process_error(msg);
    });

    d->proc->start();
    if (!d->proc->waitForStarted(5000)) {
        log_error("[TranscriptWorker] Failed to start Python subprocess: " + d->proc->errorString());
        d->proc.reset();
        return false;
    }

    log_info("[TranscriptWorker] Python live-transcribe subprocess started — PID " +
             QString::number(d->proc->processId()));
    return true;
}

void TranscriptWorker::stop() {
    if (!d->proc) return;
    d->proc->closeWriteChannel();
    if (!d->proc->waitForFinished(3000))
        d->proc->kill();
    d->proc.reset();
}

bool TranscriptWorker::is_running() const {
    return d->proc && d->proc->state() == QProcess::Running;
}

void TranscriptWorker::set_paused(bool paused) {
    if (d->paused == paused) return;
    d->paused = paused;
    emit paused_changed(paused);
}

bool TranscriptWorker::is_paused() const { return d->paused; }

void TranscriptWorker::submit_chunk(int micIndex, int sampleRate, int channels, QByteArray pcm16) {
    if (!is_running() || d->paused) return;
    if (channels <= 0 || pcm16.isEmpty()) return;

    const int sampleCount = pcm16.size() / (channels * 2);   // per-channel frame count
    if (sampleCount <= 0) return;

    // 24-byte header: mic, sampleRate, channels, sampleFormat(0=int16), sampleCount, reserved.
    QByteArray header(24, 0);
    std::memcpy(header.data() + 0,  &micIndex,   4);
    std::memcpy(header.data() + 4,  &sampleRate, 4);
    std::memcpy(header.data() + 8,  &channels,   4);
    const int sampleFormat = 0;
    std::memcpy(header.data() + 12, &sampleFormat, 4);
    std::memcpy(header.data() + 16, &sampleCount, 4);

    d->proc->write(header);
    d->proc->write(pcm16);
}

} // namespace mosaic
