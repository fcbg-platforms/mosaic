#include "ui/audio/audio_waveform_w.hpp"
#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QThread>
#include <algorithm>
#include <deque>
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
    int                              channelCount{1};
    std::vector<std::deque<float>>   history;   // one deque per channel

    void ensure_channels(int count) {
        if (count < 1) { count = 1; }
        if (count > 8) { count = 8; }
        channelCount = count;
        history.resize(static_cast<size_t>(count));
        // Pre-fill with silence so the trace starts flat.
        for (auto& ch : history) {
            while (static_cast<int>(ch.size()) < k_history) { ch.push_front(0.0f); }
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

// ── Data ingestion ─────────────────────────────────────────────────────────

void AudioWaveformW::push_level(int channelIndex, float rms) {
    // Thread safety: if called from a non-GUI thread, bounce back to GUI.
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, channelIndex, rms]{
            push_level(channelIndex, rms);
        }, Qt::QueuedConnection);
        return;
    }

    if (channelIndex < 0 || channelIndex >= d->channelCount) { return; }
    auto& ch = d->history[static_cast<size_t>(channelIndex)];
    // Boost quiet mic signals so they're visible; raw RMS is typically 0.01–0.1.
    const float gained = std::min(1.0f, rms * 90.0f);
    ch.push_back(gained);
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

        // Draw filled area first (semi-transparent)
        QPainterPath fill;
        const float xStep = static_cast<float>(rc.width()) / static_cast<float>(k_history - 1);

        fill.moveTo(0, midY);
        for (int i = 0; i < n; ++i) {
            const float xf = static_cast<float>(i) * xStep;
            const float yf = static_cast<float>(midY) - samples[static_cast<size_t>(i)] * static_cast<float>(ampH);
            fill.lineTo(static_cast<double>(xf), static_cast<double>(yf));
        }
        fill.lineTo(static_cast<double>(static_cast<float>(n - 1) * xStep), static_cast<double>(midY));
        fill.lineTo(0.0, static_cast<double>(midY));
        fill.closeSubpath();

        QColor fillClr = clr;
        fillClr.setAlpha(40);
        p.fillPath(fill, fillClr);

        // Draw line trace on top
        QPen tracePen(clr, 1.5f);
        p.setPen(tracePen);
        bool first = true;
        qreal prevX = 0.0;
        qreal prevY = 0.0;
        for (int i = 0; i < n; ++i) {
            const qreal xd = static_cast<qreal>(i) * static_cast<qreal>(xStep);
            const qreal yd = static_cast<qreal>(midY)
                           - static_cast<qreal>(samples[static_cast<size_t>(i)])
                           * static_cast<qreal>(ampH);
            if (!first) { p.drawLine(QPointF(prevX, prevY), QPointF(xd, yd)); }
            prevX = xd;
            prevY = yd;
            first = false;
        }

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
