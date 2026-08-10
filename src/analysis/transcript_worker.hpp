#pragma once
#include <QByteArray>
#include <QObject>
#include <QStringList>
#include <memory>

namespace mosaic {

// Runs the Python live-transcription subprocess (analysis/run_live_transcribe.py,
// launched against analysis/.venv — NOT python/.venv, see that script's own
// doc comment for why: faster-whisper/torch already live in the
// mosaic-analysis venv used by the post-hoc diarization pipeline, avoiding
// duplicating that heavy dependency set into python/'s otherwise-light
// mosaic-pose venv) and provides streaming speech-to-text.
//
// Mirrors PoseWorker's shape/lifecycle exactly (start/stop/is_running/
// set_paused/is_paused), but audio is a continuous stream rather than
// discrete frames — see submit_chunk()'s doc comment for the wire format.
class TranscriptWorker : public QObject {
    Q_OBJECT
public:
    explicit TranscriptWorker(QObject* parent = nullptr);
    ~TranscriptWorker() override;

    // extraArgs is appended after scriptPath (e.g. {"--model", "tiny"}) —
    // unlike PoseWorker's frame_server.py, run_live_transcribe.py takes CLI
    // args for model size, mirroring run_diarize.py's own argparse CLI.
    bool start(const QString& interpreter, const QString& scriptPath,
               const QStringList& extraArgs = {});
    void stop();
    [[nodiscard]] bool is_running() const;

    // Exact same semantics/reasoning as PoseWorker::set_paused() — does NOT
    // tear down the subprocess; submit_chunk() becomes a no-op so the
    // Python side idles on a blocking stdin read. Wired from MainWindow
    // against RecordManager::recording_started/stopped, same as PoseWorker
    // (see main_window.cpp) — a global resource policy, not owned by any tab.
    void set_paused(bool paused);
    [[nodiscard]] bool is_paused() const;

public slots:
    // pcm16 is interleaved int16 LE PCM, sampleCount * channels * 2 bytes —
    // exactly what AudioRecorder::raw_pcm_ready() emits, unconverted.
    // Written to the subprocess as a 24-byte header (mic index, sample
    // rate, channel count, sample format [always 0 = int16 today], sample
    // count, reserved) followed by the raw PCM bytes — see
    // run_live_transcribe.py's module doc comment for the exact byte layout.
    void submit_chunk(int micIndex, int sampleRate, int channels, QByteArray pcm16);

signals:
    // One call per confirmed segment (a single Python message can carry
    // more than one — see run_live_transcribe.py's new_final_segments
    // array, split into individual emits here for a simpler C++ consumer).
    void transcript_final(int micIndex, QString text);
    // Replaces (not appends) whatever tentative text was last shown for
    // this mic — may be an empty string, meaning "clear the tentative line".
    void transcript_partial(int micIndex, QString text);
    void process_error(QString message);
    void paused_changed(bool paused);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
