#pragma once
#include <QWidget>
#include <memory>

namespace mosaic {

// Read-only live captions display: a scrolling history of confirmed
// segments plus one in-progress "tentative" line that gets replaced
// wholesale (not appended) as TranscriptWorker::transcript_partial()
// fires. Deliberately not a chart — this is textual data.
class TranscriptPanelW : public QWidget {
    Q_OBJECT
public:
    explicit TranscriptPanelW(QWidget* parent = nullptr);
    ~TranscriptPanelW() override;

    // Appends a permanent, timestamped line (local receive time, not the
    // worker's audio-content-relative start_ms — see TranscriptWorker's
    // doc comment for why). Auto-scrolls only if the view was already at
    // the bottom (doesn't fight a user who scrolled up to read history).
    void push_final(const QString& text);

    // Replaces the current in-progress caption line. Empty string clears it.
    void set_tentative(const QString& text);

    void clear();

    // Static "unavailable" message shown instead of live text (venv
    // missing / subprocess failed to start) — mirrors the graceful
    // degradation PoseWorker's nullptr already gets elsewhere in this tab
    // (tiles/trace still render, they just never receive data).
    void set_unavailable(const QString& reason);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
