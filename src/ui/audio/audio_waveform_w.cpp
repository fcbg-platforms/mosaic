#include "ui/audio/audio_waveform_w.hpp"

#include <QColor>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QThread>
#include <algorithm>
#include <deque>
#include <utility>
#include <vector>

#include "ui/audio/speaker_palette.hpp"
#include "ui/audio/time_axis.hpp"

namespace mosaic {

// History length: 8 seconds * 20 samples/s
static constexpr int k_history = 160;

static constexpr QColor k_channel_colors[] = {
    QColor(0x44, 0xcc, 0x88), // 0: green
    QColor(0x88, 0x66, 0xff), // 1: purple
    QColor(0x44, 0xbb, 0xff), // 2: cyan
    QColor(0xff, 0xaa, 0x44), // 3: amber
    QColor(0xff, 0x55, 0x88), // 4: pink
    QColor(0x88, 0xff, 0x44), // 5: lime
    QColor(0xff, 0xdd, 0x55), // 6: yellow
    QColor(0xaa, 0xdd, 0xff), // 7: light blue
};

// Static-mode trace color — deliberately NOT k_channel_colors[0] (green):
// in static mode the trace represents "the mixed clip's amplitude", not a
// specific microphone channel, and once speaker bands are drawn on top,
// reusing a palette color for the trace itself would read as "the trace
// IS speaker 0" rather than "the trace is just amplitude". Distinct from
// all 8 k_channel_colors hues; reads clearly on the #08,08,16 background.
// Live mode (per-channel k_channel_colors[ch]) is untouched.
static constexpr QColor kStaticTraceColor(0x8a, 0x94, 0xb0);

// Neutral fallback returned by speaker_color() for an empty/unrecognized
// speaker label — distinct from every real palette entry.
static constexpr QColor kUnknownSpeakerColor(0x55, 0x55, 0x66);

// Height of the solid top/bottom strips framing a speaker turn. File-scope
// because the time-tick gridlines inset themselves by it so they don't run
// through the strips — the two must agree.
static constexpr qreal kBandStripHeight = 9.0;

struct AudioWaveformW::Impl {
    int channelCount{1};
    float scale{AudioWaveformW::kDefaultScale};
    // One deque per channel, each entry a raw unscaled (min, max) envelope
    // pair in [-1, 1] — display gain (scale) is applied at paint time, not
    // stored here, so it can be changed live without re-pushing history.
    std::vector<std::deque<std::pair<float, float>>> history;

    // Static mode (see set_static_envelope()): a whole clip's envelope drawn
    // across the full widget width with a movable playhead, instead of the
    // live rolling window above. Mutually exclusive with the live-mode
    // history — staticMode picks which paintEvent() branch runs.
    bool staticMode{false};
    std::vector<std::pair<float, float>> staticSamples;
    qint64 staticDurationMs{0};
    qint64 playheadMs{0};

    // Speaker-timing bands (static mode only) — see set_speaker_bands().
    // speakerOrder tracks first-appearance order (QMap<QString,int> alone
    // doesn't preserve insertion order, it sorts by key) so speaker_legend()
    // can return entries in the same order they were assigned a color.
    std::vector<AudioWaveformW::SpeakerBand> bands;
    QMap<QString, int> speakerIndex;
    QVector<QString> speakerOrder;

    std::function<void(qint64)> seekCb;

    void ensure_channels(int count) {
        if (count < 1) {
            count = 1;
        }
        if (count > 8) {
            count = 8;
        }
        channelCount = count;
        history.resize(static_cast<size_t>(count));
        // Pre-fill with silence so the trace starts flat.
        for (auto& ch : history) {
            while (static_cast<int>(ch.size()) < k_history) {
                ch.push_front({0.0f, 0.0f});
            }
        }
    }
};

// ── Constructor ────────────────────────────────────────────────────────────

AudioWaveformW::AudioWaveformW(QWidget* parent) : QWidget(parent), d(std::make_unique<Impl>()) {
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
    d->staticMode       = true;
    d->staticDurationMs = std::max<qint64>(0, durationMs);
    d->playheadMs       = 0;
    d->staticSamples.clear();
    d->staticSamples.reserve(static_cast<size_t>(envelope.size()));
    for (const auto& s : envelope) {
        d->staticSamples.push_back({s.first, s.second});
    }
    update();
}

void AudioWaveformW::set_playhead_ms(qint64 ms) {
    d->playheadMs = std::clamp<qint64>(ms, 0, d->staticDurationMs);
    if (d->staticMode) {
        update();
    }
}

void AudioWaveformW::clear_static_envelope() {
    d->staticMode = false;
    d->staticSamples.clear();
    d->staticDurationMs = 0;
    d->playheadMs       = 0;
    update();
}

// ── Speaker-timing bands ───────────────────────────────────────────────────

void AudioWaveformW::set_speaker_bands(const QVector<SpeakerBand>& bands) {
    d->bands.assign(bands.begin(), bands.end());

    QStringList orderedLabels;
    orderedLabels.reserve(bands.size());
    for (const auto& b : bands) {
        orderedLabels << b.speaker;
    }

    d->speakerIndex = assign_speaker_palette_indices(orderedLabels);
    d->speakerOrder.clear();
    d->speakerOrder.reserve(d->speakerIndex.size());
    for (const auto& label : orderedLabels) {
        if (label.isEmpty() || d->speakerOrder.contains(label)) {
            continue;
        }
        d->speakerOrder << label;
    }
    update();
}

QColor AudioWaveformW::speaker_color(const QString& speaker) const {
    if (!d->speakerIndex.contains(speaker)) {
        return kUnknownSpeakerColor;
    }
    return k_channel_colors[d->speakerIndex.value(speaker) % 8];
}

QVector<QPair<QString, QColor>> AudioWaveformW::speaker_legend() const {
    QVector<QPair<QString, QColor>> out;
    out.reserve(d->speakerOrder.size());
    for (const auto& speaker : d->speakerOrder) {
        out.append({speaker, speaker_color(speaker)});
    }
    return out;
}

void AudioWaveformW::set_seek_callback(std::function<void(qint64)> cb) {
    d->seekCb = std::move(cb);
}

// ── Data ingestion ─────────────────────────────────────────────────────────

void AudioWaveformW::push_envelope(int channelIndex, float minSample, float maxSample) {
    // Thread safety: if called from a non-GUI thread, bounce back to GUI.
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this, channelIndex, minSample, maxSample] {
                push_envelope(channelIndex, minSample, maxSample);
            },
            Qt::QueuedConnection);
        return;
    }

    if (channelIndex < 0 || channelIndex >= d->channelCount) {
        return;
    }
    auto& ch = d->history[static_cast<size_t>(channelIndex)];
    ch.push_back({minSample, maxSample});
    if (static_cast<int>(ch.size()) > k_history) {
        ch.pop_front();
    }
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
            const int topY   = 4;
            const int botY   = rc.height() - 4;
            const int midY   = (topY + botY) / 2;
            const int ampH   = (botY - topY) / 2;
            const QColor clr = kStaticTraceColor;

            const float xStep = static_cast<float>(rc.width()) / static_cast<float>(n - 1);
            auto yOf          = [&](float v) {
                const float clamped = std::clamp(v * d->scale, -1.0f, 1.0f);
                return static_cast<qreal>(midY) -
                       static_cast<qreal>(clamped) * static_cast<qreal>(ampH);
            };

            QPainterPath fill;
            fill.moveTo(0.0, yOf(d->staticSamples[0].second));
            for (int i = 1; i < n; ++i) {
                fill.lineTo(static_cast<qreal>(i) * xStep,
                            yOf(d->staticSamples[static_cast<size_t>(i)].second));
            }
            for (int i = n - 1; i >= 0; --i) {
                fill.lineTo(static_cast<qreal>(i) * xStep,
                            yOf(d->staticSamples[static_cast<size_t>(i)].first));
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
                topLine.lineTo(static_cast<qreal>(i) * xStep,
                               yOf(d->staticSamples[static_cast<size_t>(i)].second));
                botLine.lineTo(static_cast<qreal>(i) * xStep,
                               yOf(d->staticSamples[static_cast<size_t>(i)].first));
            }
            p.drawPath(topLine);
            p.drawPath(botLine);

            // Speaker-timing shading: a low-alpha full-height wash per
            // speaker turn (background context, drawn on top of the trace
            // so it tints without hiding it — doesn't compete with the
            // trace itself for attention) plus solid top- and bottom-edge
            // strips (framing the turn top-and-bottom reads more clearly as
            // a labeled "band" at a glance than a single thin top sliver).
            // Segments with no attributed speaker (gaps) draw neither — an
            // honest "no data" representation, not a fabricated color. Two
            // passes so every strip draws on top of every wash, regardless
            // of band ordering.
            if (d->staticDurationMs > 0 && !d->bands.empty()) {
                auto bandX = [&](qint64 ms) {
                    return time_to_x(ms, d->staticDurationMs, rc.width());
                };
                for (const auto& band : d->bands) {
                    if (band.speaker.isEmpty()) {
                        continue;
                    }
                    const qreal x0 = bandX(band.startMs);
                    const qreal x1 = bandX(band.endMs);
                    if (x1 <= x0) {
                        continue;
                    }
                    QColor wash = speaker_color(band.speaker);
                    // 55 was too faint to read against the #080816 ground —
                    // roughly a 21% tint, which on a dark background is close
                    // to invisible at a glance, and the whole point of the
                    // band is to be seen at a glance.
                    wash.setAlpha(80);
                    p.fillRect(QRectF(x0, 0, x1 - x0, rc.height()), wash);
                }
                for (const auto& band : d->bands) {
                    if (band.speaker.isEmpty()) {
                        continue;
                    }
                    const qreal x0 = bandX(band.startMs);
                    const qreal x1 = bandX(band.endMs);
                    if (x1 <= x0) {
                        continue;
                    }
                    const QColor strip = speaker_color(band.speaker);
                    p.fillRect(QRectF(x0, 0, x1 - x0, kBandStripHeight), strip);
                    p.fillRect(
                        QRectF(x0, rc.height() - kBandStripHeight, x1 - x0, kBandStripHeight),
                        strip);

                    // Name the speaker inside the band when it is wide enough
                    // to hold the text. Colour alone forces a lookup against
                    // the legend and a count of which hue is which; a label
                    // removes that step for the long turns, which are the ones
                    // worth identifying. Short turns keep colour only rather
                    // than get a clipped or overlapping label.
                    const qreal bandWidth = x1 - x0;
                    const QString label   = band.speaker;
                    QFont labelFont       = p.font();
                    // 8, not 10: drawText(QRectF, ...) clips to the rect, and
                    // a 10px line box (ascent+descent ~12px) does not fit the
                    // 9px strip — the underscore in "SPEAKER_00" is the first
                    // thing to vanish. Paired with TextDontClip below so a
                    // descender is never sheared even at this size.
                    labelFont.setPixelSize(8);
                    labelFont.setBold(true);
                    // Measured with the font it is actually drawn in, not the
                    // painter's current one — otherwise the fit test is against
                    // the wrong metrics and a label can overflow its band.
                    const int textW = QFontMetrics(labelFont).horizontalAdvance(label);
                    if (bandWidth >= textW + 12) {
                        p.save();
                        p.setFont(labelFont);
                        // Dark text on the solid strip, which is a bright
                        // palette colour — a light label would disappear.
                        p.setPen(QColor(0x0a, 0x0a, 0x18, 220));
                        p.drawText(QRectF(x0 + 5, 0, bandWidth - 10, kBandStripHeight),
                                   Qt::AlignVCenter | Qt::AlignLeft | Qt::TextDontClip, label);
                        p.restore();
                    }
                }
            }

            // Time ticks. Without them this strip shows *that* the speakers
            // alternate but not *when*, so reading a turn off it means
            // scrubbing the playhead until the readout matches — which
            // defeats the point of an overview. Drawn after the bands so the
            // labels stay legible over a coloured wash, before the playhead
            // so the playhead stays on top.
            if (d->staticDurationMs > 0) {
                // Aim for a tick roughly every 90 px, snapped to a round
                // interval — a "nice" number of seconds, not width/8, so the
                // labels read as clock times rather than arbitrary offsets.
                static constexpr qint64 kNiceStepsMs[] = {1000,   2000,    5000,   10000,  15000,
                                                          30000,  60000,   120000, 300000, 600000,
                                                          900000, 1800000, 3600000};
                const qreal targetTicks                = std::max(2.0, rc.width() / 90.0);
                qint64 step                            = kNiceStepsMs[std::size(kNiceStepsMs) - 1];
                for (const qint64 candidate : kNiceStepsMs) {
                    if (static_cast<qreal>(d->staticDurationMs) / candidate <= targetTicks) {
                        step = candidate;
                        break;
                    }
                }

                QFont tickFont = p.font();
                tickFont.setPixelSize(9);
                p.setFont(tickFont);
                for (qint64 t = step; t < d->staticDurationMs; t += step) {
                    const qreal x = time_to_x(t, d->staticDurationMs, rc.width());
                    p.setPen(QPen(QColor(0xff, 0xff, 0xff, 26), 1, Qt::DotLine));
                    p.drawLine(QPointF(x, kBandStripHeight),
                               QPointF(x, rc.height() - kBandStripHeight));

                    const qint64 totalSec = t / 1000;
                    const QString label =
                        QString("%1:%2").arg(totalSec / 60).arg(totalSec % 60, 2, 10, QChar('0'));
                    const int tw = QFontMetrics(tickFont).horizontalAdvance(label);
                    // Ticks run to just short of the duration, so the last one
                    // can sit close enough to the right edge that its label
                    // would be sheared mid-glyph. Draw the gridline regardless
                    // — it still carries position — and drop only the text.
                    if (x + tw + 6 > rc.width()) {
                        continue;
                    }
                    // Backing plate, so a label over a bright band or a dense
                    // part of the trace stays readable.
                    p.fillRect(QRectF(x + 2, rc.height() / 2.0 - 6, tw + 4, 12),
                               QColor(0x08, 0x08, 0x16, 170));
                    p.setPen(QColor(0x8a, 0x8a, 0xb4));
                    p.drawText(QRectF(x + 4, rc.height() / 2.0 - 6, tw, 12),
                               Qt::AlignVCenter | Qt::AlignLeft, label);
                }
            }

            // Playhead: bright vertical line at the current position, in
            // place of live mode's fixed right-edge "now" indicator.
            if (d->staticDurationMs > 0) {
                const qreal x = time_to_x(d->playheadMs, d->staticDurationMs, rc.width());
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
        const int topY      = ch * chH + chGap;
        const int botY      = topY + chH - chGap * 2;
        const int midY      = (topY + botY) / 2;
        const int ampH      = (botY - topY) / 2;

        const QColor clr = (ch < 8) ? k_channel_colors[ch] : QColor(0xaa, 0xaa, 0xcc);

        const int n = static_cast<int>(samples.size());
        if (n < 2) {
            continue;
        }

        const float xStep = static_cast<float>(rc.width()) / static_cast<float>(k_history - 1);

        // Maps a raw [-1,1] sample to its y-coordinate, applying the
        // current display scale and clamping so an over-scaled signal
        // flattens against the channel strip's edges instead of drawing
        // outside it.
        auto yOf = [&](float v) {
            const float clamped = std::clamp(v * d->scale, -1.0f, 1.0f);
            return static_cast<qreal>(midY) -
                   static_cast<qreal>(clamped) * static_cast<qreal>(ampH);
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
            topLine.lineTo(static_cast<qreal>(i) * xStep,
                           yOf(samples[static_cast<size_t>(i)].second));
            botLine.lineTo(static_cast<qreal>(i) * xStep,
                           yOf(samples[static_cast<size_t>(i)].first));
        }
        p.drawPath(topLine);
        p.drawPath(botLine);

        // Channel label in top-left of the channel strip
        p.setPen(clr.darker(120));
        p.setFont(QFont("monospace", 8));
        p.drawText(QRect(4, topY, 30, 12), Qt::AlignLeft | Qt::AlignTop, QString("Ch%1").arg(ch));
    }

    // Right-edge "now" indicator
    p.setPen(QPen(QColor(0x44, 0x44, 0x66), 1, Qt::DashLine));
    p.drawLine(rc.width() - 1, 0, rc.width() - 1, rc.height());
}

void AudioWaveformW::mousePressEvent(QMouseEvent* event) {
    if (!d->staticMode || !d->seekCb || d->staticDurationMs <= 0) {
        QWidget::mousePressEvent(event);
        return;
    }
    d->seekCb(x_to_time(event->pos().x(), d->staticDurationMs, width()));
    QWidget::mousePressEvent(event);
}

QSize AudioWaveformW::sizeHint() const {
    // Height: 40px per channel, min 40, max 240
    const int h = qBound(40, d->channelCount * 40, 240);
    return {400, h};
}

} // namespace mosaic
