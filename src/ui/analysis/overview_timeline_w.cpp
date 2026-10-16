#include "ui/analysis/overview_timeline_w.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <algorithm>
#include <cmath>

namespace mosaic {

namespace {

constexpr int kLabelWidth = 130; // lane names
constexpr int kAxisHeight = 26;  // time labels under the lanes
constexpr int kLaneGap    = 6;
const QColor kBackground("#0e0e22");
const QColor kLaneBackground("#14142c");
const QColor kGrid("#1e1e3a");
const QColor kText("#a0a0c8");
const QColor kMuted("#6060a0");
const QColor kPlayhead("#ff4466");

// Who speaks: on camera, other. Eye contact: looking, away.
QColor span_color(const QString& laneId, int tag) {
    if (laneId == "speaking") {
        return tag == 0 ? QColor("#5ac85a") : QColor("#3ca0e6");
    }
    return tag == 0 ? QColor("#44cc88") : QColor("#3a3a5a");
}

QColor line_color(const QString& laneId) {
    if (laneId == "smile") {
        return QColor("#e6c040");
    }
    if (laneId == "head") {
        return QColor("#b07ce6");
    }
    return QColor("#ff5577"); // heart rate
}

// Tick spacing giving roughly one label per 90 px.
double tick_step_ms(double durationMs, double widthPx) {
    const double target = durationMs * 90.0 / std::max(1.0, widthPx);
    for (double s : {1000.0, 2000.0, 5000.0, 10000.0, 15000.0, 30000.0, 60000.0, 120000.0, 300000.0,
                     600000.0, 900000.0, 1800000.0}) {
        if (s >= target) {
            return s;
        }
    }
    return 3600000.0;
}

QString time_label(double ms) {
    const int s = static_cast<int>(std::round(ms / 1000.0));
    return s >= 60 ? QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'))
                   : QString("%1 s").arg(s);
}

} // namespace

OverviewTimelineW::OverviewTimelineW(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumHeight(260);
    setCursor(Qt::PointingHandCursor);
}

void OverviewTimelineW::set_overview(const SessionOverview& overview) {
    overview_ = overview;
    update();
}

void OverviewTimelineW::set_playhead_ms(qint64 ms) {
    playheadMs_ = ms;
    update();
}

QRectF OverviewTimelineW::plot_rect() const {
    return QRectF(kLabelWidth, 8, std::max(10, width() - kLabelWidth - 12),
                  std::max(10, height() - 8 - kAxisHeight));
}

double OverviewTimelineW::x_of(double ms) const {
    const QRectF r = plot_rect();
    const double d = std::max(1.0, overview_.durationMs);
    return r.left() + std::clamp(ms / d, 0.0, 1.0) * r.width();
}

double OverviewTimelineW::ms_of(double x) const {
    const QRectF r = plot_rect();
    return std::clamp((x - r.left()) / r.width(), 0.0, 1.0) * std::max(0.0, overview_.durationMs);
}

void OverviewTimelineW::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), kBackground);
    const QRectF plot = plot_rect();
    const int n       = static_cast<int>(overview_.lanes.size());
    if (n == 0 || overview_.durationMs <= 0.0) {
        p.setPen(kMuted);
        p.drawText(rect(), Qt::AlignCenter,
                   "No analysis for this camera yet: run Conversation Timing, Eye Contact, "
                   "Face Dynamics or Remote Heart Rate.");
        return;
    }

    // Time grid and labels.
    const double step = tick_step_ms(overview_.durationMs, plot.width());
    QFont small       = font();
    small.setPointSize(9);
    p.setFont(small);
    for (double t = 0.0; t <= overview_.durationMs + 1.0; t += step) {
        const double x = x_of(t);
        p.setPen(QPen(kGrid, 1));
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        p.setPen(kMuted);
        p.drawText(QRectF(x - 40, plot.bottom() + 4, 80, kAxisHeight - 6), Qt::AlignHCenter,
                   time_label(t));
    }

    const double laneH = (plot.height() - kLaneGap * (n - 1)) / n;
    QFont labelFont    = font();
    labelFont.setPointSize(10);
    for (int i = 0; i < n; ++i) {
        const OverviewLane& lane = overview_.lanes[i];
        const QRectF lr(plot.left(), plot.top() + i * (laneH + kLaneGap), plot.width(), laneH);
        p.fillRect(lr, kLaneBackground);

        p.setFont(labelFont);
        p.setPen(kText);
        p.drawText(QRectF(8, lr.top(), kLabelWidth - 14, lr.height() * 0.6),
                   Qt::AlignRight | Qt::AlignBottom, lane.name);
        if (lane.kind == OverviewLane::Kind::Line && lane.note.isEmpty()) {
            p.setFont(small);
            p.setPen(kMuted);
            const QString range = QString("%1 to %2%3")
                                      .arg(lane.minY, 0, 'g', 3)
                                      .arg(lane.maxY, 0, 'g', 3)
                                      .arg(lane.unit.isEmpty() ? QString() : " " + lane.unit);
            p.drawText(QRectF(8, lr.top() + lr.height() * 0.6, kLabelWidth - 14, lr.height() * 0.4),
                       Qt::AlignRight | Qt::AlignTop, range);
        }

        if (!lane.note.isEmpty()) {
            p.setFont(small);
            p.setPen(kMuted);
            p.drawText(lr, Qt::AlignCenter, lane.note);
            continue;
        }
        if (lane.kind == OverviewLane::Kind::Spans) {
            for (const auto& s : lane.spans) {
                const double a = x_of(s.startMs);
                const double b = std::max(a + 1.0, x_of(s.endMs));
                // Who speaks: on camera in the upper half, other in the
                // lower, so overlaps show as both.
                QRectF r(a, lr.top() + 3, b - a, lr.height() - 6);
                if (lane.id == "speaking") {
                    r = s.tag == 0 ? QRectF(a, lr.top() + 3, b - a, lr.height() / 2 - 4)
                                   : QRectF(a, lr.center().y() + 1, b - a, lr.height() / 2 - 4);
                }
                p.fillRect(r, span_color(lane.id, s.tag));
            }
            continue;
        }
        // A line, broken where a value is missing.
        const double span = std::max(1e-9, lane.maxY - lane.minY);
        QPainterPath path;
        bool open = false;
        for (const auto& pt : lane.points) {
            if (!std::isfinite(pt.y())) {
                open = false;
                continue;
            }
            const double y = lr.bottom() - 3 -
                             std::clamp((pt.y() - lane.minY) / span, 0.0, 1.0) * (lr.height() - 6);
            const QPointF q(x_of(pt.x()), y);
            if (open) {
                path.lineTo(q);
            } else {
                path.moveTo(q);
                open = true;
            }
        }
        p.setPen(QPen(line_color(lane.id), 1.6));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }

    // Playhead across every lane.
    const double px = x_of(static_cast<double>(playheadMs_));
    p.setPen(QPen(kPlayhead, 1.5));
    p.drawLine(QPointF(px, plot.top()), QPointF(px, plot.bottom()));
    if (hoverX_ >= plot.left() && hoverX_ <= plot.right()) {
        p.setPen(QPen(QColor(255, 255, 255, 60), 1, Qt::DashLine));
        p.drawLine(QPointF(hoverX_, plot.top()), QPointF(hoverX_, plot.bottom()));
    }
}

void OverviewTimelineW::mousePressEvent(QMouseEvent* e) {
    if (seek_ && overview_.durationMs > 0.0 && plot_rect().contains(e->position())) {
        seek_(static_cast<qint64>(ms_of(e->position().x())));
    }
    QWidget::mousePressEvent(e);
}

void OverviewTimelineW::mouseMoveEvent(QMouseEvent* e) {
    hoverX_ = e->position().x();
    update();
    if (overview_.durationMs <= 0.0 || !plot_rect().contains(e->position())) {
        QToolTip::hideText();
        return;
    }
    // The time, and each lane's value there.
    const double ms = ms_of(hoverX_);
    QStringList lines{time_label(ms) + QString(" (%1 s)").arg(ms / 1000.0, 0, 'f', 1)};
    for (const auto& lane : overview_.lanes) {
        if (!lane.note.isEmpty()) {
            continue;
        }
        if (lane.kind == OverviewLane::Kind::Spans) {
            QStringList here;
            for (const auto& s : lane.spans) {
                if (ms >= s.startMs && ms < s.endMs) {
                    here << (lane.id == "speaking" ? (s.tag == 0 ? "on camera" : "other")
                                                   : (s.tag == 0 ? "looking" : "away"));
                }
            }
            lines << QString("%1: %2").arg(lane.name, here.isEmpty() ? "-" : here.join(", "));
        } else {
            // The last point at or before this time.
            double v = std::numeric_limits<double>::quiet_NaN();
            for (const auto& pt : lane.points) {
                if (pt.x() > ms) {
                    break;
                }
                v = pt.y();
            }
            lines << QString("%1: %2%3")
                         .arg(lane.name, std::isfinite(v) ? QString::number(v, 'f', 2) : "-")
                         .arg(std::isfinite(v) && !lane.unit.isEmpty() ? " " + lane.unit : "");
        }
    }
    QToolTip::showText(e->globalPosition().toPoint(), lines.join("\n"), this);
}

void OverviewTimelineW::leaveEvent(QEvent*) {
    hoverX_ = -1.0;
    QToolTip::hideText();
    update();
}

} // namespace mosaic
