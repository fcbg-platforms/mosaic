#pragma once
#include <QWidget>
#include <memory>

namespace mosaic {

// A live, continuously-scrolling trace of one keypoint's normalized (x, y)
// position over the last ~15 seconds — the real-time counterpart to the
// post-hoc Analysis tab's Position chart, but fed incrementally as
// PoseWorker::pose_ready samples arrive rather than loaded from a saved
// .pose.json all at once. Pure QPainter (not Qt Charts), matching this
// project's established split: Qt Charts for post-hoc whole-dataset plots,
// raw QPainter for high-frequency/streaming views (see AudioWaveformW).
//
// Coordinates are MediaPipe's own normalized [0,1] convention (fraction of
// the source frame), matching RealtimeCameraTileW's ThumbnailAreaW overlay
// exactly — the same numbers a hover here reports are the same numbers
// driving the dot moving over the video above it.
class RealtimeTraceW : public QWidget {
    Q_OBJECT
   public:
    explicit RealtimeTraceW(QWidget* parent = nullptr);
    ~RealtimeTraceW() override;

    // Appends one (x, y) sample at wall-clock time nowMs. Samples older than
    // the rolling window are dropped automatically on the next paint.
    void push_sample(qint64 nowMs, double x, double y);

    // Clears all history and returns to the "No data yet" empty state —
    // call when the selected camera or keypoint changes, so a stale trace
    // from the previous selection never lingers.
    void clear();

   protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

   private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
