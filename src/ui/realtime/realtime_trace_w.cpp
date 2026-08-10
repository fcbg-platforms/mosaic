#include "ui/realtime/realtime_trace_w.hpp"
#include "ui/anim_utils.hpp"
#include <QDateTime>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <deque>
#include <limits>

namespace mosaic {

namespace {

// Same cyan already painted for the live pose keypoint dots
// (RealtimeCameraTileW::ThumbnailAreaW::paint_pose(), QColor(0,220,255)) —
// reused verbatim rather than a fresh palette pick, so the moving dot on
// the video feed and this trace read as the same signal, not two
// independently-colored features.
constexpr QColor kTraceColor(0, 220, 255);
constexpr QColor kGridColor(255, 255, 255, 18);      // recessive, barely-there
constexpr QColor kAxisTextColor(0x70, 0x70, 0xa0);
constexpr QColor kBackgroundColor(0x0a, 0x0a, 0x1a);

constexpr qint64 kWindowMs = 15'000;   // rolling window shown at once

struct Sample { qint64 ms; double x; double y; };

} // namespace

struct RealtimeTraceW::Impl {
    std::deque<Sample> samples;
    bool    hasEverHadData = false;
    QPointF hoverPos;          // widget-space; valid only while hovering
    bool    hovering = false;
};

RealtimeTraceW::RealtimeTraceW(QWidget* parent)
    : QWidget(parent), d(std::make_unique<Impl>())
{
    setMinimumHeight(140);
    setMouseTracking(true);
}

RealtimeTraceW::~RealtimeTraceW() = default;

void RealtimeTraceW::push_sample(qint64 nowMs, double x, double y) {
    d->samples.push_back({nowMs, x, y});
    while (!d->samples.empty() && nowMs - d->samples.front().ms > kWindowMs) {
        d->samples.pop_front();
    }
    if (!d->hasEverHadData) {
        d->hasEverHadData = true;
        anim::fade_in_widget(this, 200);
    }
    update();
}

void RealtimeTraceW::clear() {
    d->samples.clear();
    d->hovering = false;
    update();
}

void RealtimeTraceW::mouseMoveEvent(QMouseEvent* event) {
    d->hovering = true;
    d->hoverPos = event->position();
    update();
}

void RealtimeTraceW::leaveEvent(QEvent*) {
    d->hovering = false;
    update();
}

void RealtimeTraceW::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF rc = rect();
    p.fillRect(rc, kBackgroundColor);

    // Plot area — small margins for the recessive axis labels, a larger one
    // on the right so the live edge's end-marker never clips.
    constexpr double kMarginL = 28, kMarginR = 14, kMarginT = 10, kMarginB = 18;
    const QRectF plot(rc.left() + kMarginL, rc.top() + kMarginT,
                       rc.width() - kMarginL - kMarginR,
                       rc.height() - kMarginT - kMarginB);

    // Recessive gridlines + normalized-position axis labels at 0 / 0.5 / 1.
    p.setPen(QPen(kGridColor, 1));
    for (const double frac : {0.0, 0.5, 1.0}) {
        const double y = plot.bottom() - frac * plot.height();
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    p.setPen(kAxisTextColor);
    QFont axisFont = p.font();
    axisFont.setPointSize(8);
    p.setFont(axisFont);
    for (const double frac : {0.0, 0.5, 1.0}) {
        const double y = plot.bottom() - frac * plot.height();
        p.drawText(QRectF(rc.left(), y - 7, kMarginL - 4, 14),
                    Qt::AlignRight | Qt::AlignVCenter, QString::number(frac, 'f', 1));
    }

    // Trimmed against real wall-clock time (not just the most recent pushed
    // sample) so the trace actually scrolls/empties out within one window
    // if its camera stops sending data (Analyze unchecked, or the shared
    // pose pipeline paused for a recording) — otherwise it would freeze on
    // its last few samples forever, the same staleness bug already fixed
    // once for RealtimeCameraTileW's own overlay.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    while (!d->samples.empty() && nowMs - d->samples.front().ms > kWindowMs) {
        d->samples.pop_front();
    }

    if (d->samples.empty()) {
        p.setPen(kAxisTextColor);
        QFont f = p.font();
        f.setPointSize(10);
        p.setFont(f);
        p.drawText(rc, Qt::AlignCenter, "No data yet");
        return;
    }

    const auto to_widget = [&](const Sample& s, double value) {
        const double tFrac = 1.0 - static_cast<double>(nowMs - s.ms) / kWindowMs;   // 0=left(oldest), 1=right(now)
        return QPointF(plot.left() + tFrac * plot.width(),
                        plot.bottom() - value * plot.height());
    };

    QPainterPath xPath, yPath;
    for (size_t i = 0; i < d->samples.size(); ++i) {
        const auto& s = d->samples[i];
        const QPointF px = to_widget(s, s.x);
        const QPointF py = to_widget(s, s.y);
        if (i == 0) { xPath.moveTo(px); yPath.moveTo(py); }
        else        { xPath.lineTo(px); yPath.lineTo(py); }
    }

    p.setPen(QPen(kTraceColor, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(xPath);
    p.setPen(QPen(kTraceColor, 2, Qt::DashLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(yPath);

    // Live-edge end markers — a small filled dot at the most recent sample
    // of each series, drawing the eye to "this end is now, still moving."
    const Sample& last = d->samples.back();
    p.setPen(Qt::NoPen);
    p.setBrush(kTraceColor);
    p.drawEllipse(to_widget(last, last.x), 4, 4);
    p.drawEllipse(to_widget(last, last.y), 4, 4);

    // Legend — a manual two-entry line-style key (solid = X, dashed = Y):
    // both series share one hue, so this distinguishes them by stroke, not
    // color, and the label text stays neutral ink per this app's own
    // established "text never wears the series color" convention.
    const double legendY = rc.top() + 4;
    double legendX = plot.left();
    const auto draw_legend_entry = [&](Qt::PenStyle style, const QString& label) {
        p.setPen(QPen(kTraceColor, 2, style, Qt::RoundCap));
        p.drawLine(QPointF(legendX, legendY + 5), QPointF(legendX + 16, legendY + 5));
        legendX += 20;
        p.setPen(kAxisTextColor);
        p.drawText(QRectF(legendX, legendY, 40, 12), Qt::AlignLeft | Qt::AlignVCenter, label);
        legendX += 26;
    };
    draw_legend_entry(Qt::SolidLine, "X");
    draw_legend_entry(Qt::DashLine, "Y");

    // Hover crosshair + tooltip-style readout — nearest sample to the
    // cursor's x position, matching the dataviz guidance that a line chart
    // ships hover feedback by default rather than only a bare visual trace.
    if (d->hovering && plot.contains(d->hoverPos)) {
        size_t nearest = 0;
        double bestDist = std::numeric_limits<double>::max();
        for (size_t i = 0; i < d->samples.size(); ++i) {
            const double x = to_widget(d->samples[i], 0.0).x();
            const double dist = std::abs(x - d->hoverPos.x());
            if (dist < bestDist) { bestDist = dist; nearest = i; }
        }
        const Sample& s = d->samples[nearest];
        const double crossX = to_widget(s, 0.0).x();
        p.setPen(QPen(QColor(255, 255, 255, 60), 1, Qt::DashLine));
        p.drawLine(QPointF(crossX, plot.top()), QPointF(crossX, plot.bottom()));

        const QString text = QString("t=%1s   x=%2   y=%3")
            .arg((s.ms - nowMs) / 1000.0, 0, 'f', 1)
            .arg(s.x, 0, 'f', 2)
            .arg(s.y, 0, 'f', 2);
        QFont f = p.font();
        f.setPointSize(9);
        p.setFont(f);
        const QFontMetrics fm(f);
        const QRectF box(std::min(crossX + 6, rc.right() - fm.horizontalAdvance(text) - 12),
                          plot.top() + 2, fm.horizontalAdvance(text) + 10, 18);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x1a, 0x1a, 0x38, 235));
        p.drawRoundedRect(box, 3, 3);
        p.setPen(QColor(0xc8, 0xc8, 0xe0));
        p.drawText(box, Qt::AlignCenter, text);
    }
}

} // namespace mosaic
