#include "ui/audio/audio_waveform_w.hpp"
#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QThread>
#include <algorithm>
#include <deque>
#include <utility>
#include <vector>

namespace mosaic {

// History length: 8 seconds * 20 samples/s
static constexpr int k_history = 160;

static constexpr QColor k_channel_colors[] = {
    QColor(0x44, 0xcc, 0x88),   // 0: green
    QColor(0x88, 0x66, 0xff),   // 1: purple
    QColor(0x44, 0xbb, 0xff),   // 2: cyan
    QColor(0xff, 0xaa, 0x44),   // 3: amber
    QColor(0xff, 0x55, 0x88),   // 4: pink
    QColor(0x88, 0xff, 0x44),   // 5: lime
    QColor(0xff, 0xdd, 0x55),   // 6: yellow
    QColor(0xaa, 0xdd, 0xff),   // 7: light blue
};

struct AudioWaveformW::Impl {
    int    channelCount{1};
    float  scale{AudioWaveformW::kDefaultScale};
    // One deque per channel, each entry a raw unscaled (min, max) envelope
    // pair in [-1, 1] — display gain (scale) is applied at paint time, not
    // stored here, so it can be changed live without re-pushing history.
    std::vector<std::deque<std::pair<float, float>>> history;

    // Static mode (see set_static_envelope()): a whole clip's envelope drawn
    // across the full widget width with a movable playhead, instead of the
    // live rolling window above. Mutually exclusive with the live-mode
    // history — staticMode picks which paintEvent() branch runs.
    bool                              staticMode{false};
    std::vector<std::pair<float, float>> staticSamples;
    qint64                           staticDurationMs{0};
    qint64                           playheadMs{0};

    void ensure_channels(int count) {
        if (count < 1) { count = 1; }
        if (count > 8) { count = 8; }
        channelCount = count;
        history.resize(static_cast<size_t>(count));
        // Pre-fill with silence so the trace starts flat.
        for (auto& ch : history) {
            while (static_cast<int>(ch.size()) < k_history) { ch.push_front({0.0f, 0.0f}); }
        }
    }
};

// ── Constructor ────────────────────────────────────────────────────────────

AudioWaveformW::AudioWaveformW(QWidget* parent)
    : QWidget(parent), d(std::make_unique<Impl>())
{
    d->ensure_channels(1);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

AudioWaveformW::~AudioWaveformW() = default;

// ── Channel management ─────────────────────────────────────────────────────

void AudioWaveformW::set_channel_count(int count) {
    d->ensure_channels(count);
    update();
}

int AudioWaveformW::channel_count() const { return d->channelCount; }

// ── Scale ──────────────────────────────────────────────────────────────────

void AudioWaveformW::set_scale(float scale) {
    d->scale = std::clamp(scale, kMinScale, kMaxScale);
    update();
}

float AudioWaveformW::scale() const { return d->scale; }

// ── Static mode ────────────────────────────────────────────────────────────

void AudioWaveformW::set_static_envelope(const QVector<QPair<float, float>>& envelope,
                                          qint64 durationMs) {
    d->staticMode      = true;
    d->staticDurationMs = std::max<qint64>(0, durationMs);
    d->playheadMs      = 0;
    d->staticSamples.clear();
    d->staticSamples.reserve(static_cast<size_t>(envelope.size()));
    for (const auto& s : envelope) { d->staticSamples.push_back({s.first, s.second}); }
    update();
}

void AudioWaveformW::set_playhead_ms(qint64 ms) {
    d->playheadMs = std::clamp<qint64>(ms, 0, d->staticDurationMs);
    if (d->staticMode) { update(); }
}

void AudioWaveformW::clear_static_envelope() {
    d->staticMode = false;
    d->staticSamples.clear();
    d->staticDurationMs = 0;
    d->playheadMs = 0;
    update();
}

// ── Data ingestion ─────────────────────────────────────────────────────────

void AudioWaveformW::push_envelope(int channelIndex, float minSample, float maxSample) {
    // Thread safety: if called from a non-GUI thread, bounce back to GUI.
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, channelIndex, minSample, maxSample]{
            push_envelope(channelIndex, minSample, maxSample);
        }, Qt::QueuedConnection);
        return;
    }

    if (channelIndex < 0 || channelIndex >= d->channelCount) { return; }
    auto& ch = d->history[static_cast<size_t>(channelIndex)];
    ch.push_back({minSample, maxSample});
    if (static_cast<int>(ch.size()) > k_history) { ch.pop_front(); }
    update();
}

// ── Paint ──────────────────────────────────────────────────────────────────

void AudioWaveformW::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRect rc = rect();
    p.fillRect(rc, QColor(0x08, 0x08, 0x16));

    // Draw subtle grid lines
    p.setPen(QPen(QColor(0x18, 0x18, 0x30), 1));
    const int gridLines = 4;
    for (int i = 1; i < gridLines; ++i) {
        const int y = rc.height() * i / gridLines;
        p.drawLine(0, y, rc.width(), y);
    }

    if (d->staticMode) {
        const int n = static_cast<int>(d->staticSamples.size());
        if (n >= 2) {
            const int topY = 4;
            const int botY = rc.height() - 4;
            const int midY = (topY + botY) / 2;
            const int ampH = (botY - topY) / 2;
            const QColor clr = k_channel_colors[0];

            const float xStep = static_cast<float>(rc.width()) / static_cast<float>(n - 1);
            auto yOf = [&](float v) {
                const float clamped = std::clamp(v * d->scale, -1.0f, 1.0f);
                return static_cast<qreal>(midY)
                     - static_cast<qreal>(clamped) * static_cast<qreal>(ampH);
            };

            QPainterPath fill;
            fill.moveTo(0.0, yOf(d->staticSamples[0].second));
            for (int i = 1; i < n; ++i) {
                fill.lineTo(static_cast<qreal>(i) * xStep, yOf(d->staticSamples[static_cast<size_t>(i)].second));
            }
            for (int i = n - 1; i >= 0; --i) {
                fill.lineTo(static_cast<qreal>(i) * xStep, yOf(d->staticSamples[static_cast<size_t>(i)].first));
            }
            fill.closeSubpath();

            QColor fillClr = clr;
            fillClr.setAlpha(50);
            p.fillPath(fill, fillClr);

            QPen tracePen(clr, 1.3f);
            p.setPen(tracePen);
            QPainterPath topLine, botLine;
            topLine.moveTo(0.0, yOf(d->staticSamples[0].second));
            botLine.moveTo(0.0, yOf(d->staticSamples[0].first));
            for (int i = 1; i < n; ++i) {
                topLine.lineTo(static_cast<qreal>(i) * xStep, yOf(d->staticSamples[static_cast<size_t>(i)].second));
                botLine.lineTo(static_cast<qreal>(i) * xStep, yOf(d->staticSamples[static_cast<size_t>(i)].first));
            }
            p.drawPath(topLine);
            p.drawPath(botLine);

            // Playhead: bright vertical line at the current position, in
            // place of live mode's fixed right-edge "now" indicator.
            if (d->staticDurationMs > 0) {
                const qreal frac = std::clamp<qreal>(
                    static_cast<qreal>(d->playheadMs) / static_cast<qreal>(d->staticDurationMs),
                    0.0, 1.0);
                const qreal x = frac * rc.width();
                p.setPen(QPen(QColor(0xff, 0xdd, 0x55), 2));
                p.drawLine(QPointF(x, 0), QPointF(x, rc.height()));
            }
        }
        return;
    }

    // Channel height: divide evenly, with a small gap
    const int chH   = d->channelCount > 0 ? (rc.height() / d->channelCount) : rc.height();
    const int chGap = 2;

    for (int ch = 0; ch < d->channelCount; ++ch) {
        const auto& samples = d->history[static_cast<size_t>(ch)];
        const int   topY    = ch * chH + chGap;
        const int   botY    = topY + chH - chGap * 2;
        const int   midY    = (topY + botY) / 2;
        const int   ampH    = (botY - topY) / 2;

        const QColor clr = (ch < 8)
            ? k_channel_colors[ch]
            : QColor(0xaa, 0xaa, 0xcc);

        const int n = static_cast<int>(samples.size());
        if (n < 2) { continue; }

        const float xStep = static_cast<float>(rc.width()) / static_cast<float>(k_history - 1);

        // Maps a raw [-1,1] sample to its y-coordinate, applying the
        // current display scale and clamping so an over-scaled signal
        // flattens against the channel strip's edges instead of drawing
        // outside it.
        auto yOf = [&](float v) {
            const float clamped = std::clamp(v * d->scale, -1.0f, 1.0f);
            return static_cast<qreal>(midY) - static_cast<qreal>(clamped) * static_cast<qreal>(ampH);
        };

        // Filled area: top edge walks the max series left→right, bottom
        // edge walks the min series right→left, closing a real bipolar
        // envelope polygon (both positive and negative excursions), not an
        // always-upward-from-midline shape.
        QPainterPath fill;
        fill.moveTo(0.0, yOf(samples[0].second));
        for (int i = 1; i < n; ++i) {
            fill.lineTo(static_cast<qreal>(i) * xStep, yOf(samples[static_cast<size_t>(i)].second));
        }
        for (int i = n - 1; i >= 0; --i) {
            fill.lineTo(static_cast<qreal>(i) * xStep, yOf(samples[static_cast<size_t>(i)].first));
        }
        fill.closeSubpath();

        QColor fillClr = clr;
        fillClr.setAlpha(40);
        p.fillPath(fill, fillClr);

        // Outline: max and min traces drawn as two strokes.
        QPen tracePen(clr, 1.5f);
        p.setPen(tracePen);
        QPainterPath topLine;
        QPainterPath botLine;
        topLine.moveTo(0.0, yOf(samples[0].second));
        botLine.moveTo(0.0, yOf(samples[0].first));
        for (int i = 1; i < n; ++i) {
            topLine.lineTo(static_cast<qreal>(i) * xStep, yOf(samples[static_cast<size_t>(i)].second));
            botLine.lineTo(static_cast<qreal>(i) * xStep, yOf(samples[static_cast<size_t>(i)].first));
        }
        p.drawPath(topLine);
        p.drawPath(botLine);

        // Channel label in top-left of the channel strip
        p.setPen(clr.darker(120));
        p.setFont(QFont("monospace", 8));
        p.drawText(QRect(4, topY, 30, 12),
                   Qt::AlignLeft | Qt::AlignTop,
                   QString("Ch%1").arg(ch));
    }

    // Right-edge "now" indicator
    p.setPen(QPen(QColor(0x44, 0x44, 0x66), 1, Qt::DashLine));
    p.drawLine(rc.width() - 1, 0, rc.width() - 1, rc.height());
}

QSize AudioWaveformW::sizeHint() const {
    // Height: 40px per channel, min 40, max 240
    const int h = qBound(40, d->channelCount * 40, 240);
    return {400, h};
}

} // namespace mosaic
