#include "ui/analysis/gaze_room_view_w.hpp"

#include <QPainter>
#include <algorithm>
#include <cmath>
#include <limits>

#include "video/camera_label.hpp"

namespace mosaic {

namespace {

// Same per-subject colours as the annotated videos (analysis/gaze/render.py
// PALETTE, which is BGR there).
const QColor kSubjectColors[] = {
    QColor(255, 200, 60), QColor(40, 160, 255), QColor(90, 230, 90),
    QColor(230, 90, 230), QColor(90, 255, 255), QColor(255, 120, 80),
};

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 mul(const Vec3& a, double k) { return {a[0] * k, a[1] * k, a[2] * k}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec3 unit(const Vec3& a) {
    const double n = std::sqrt(dot(a, a));
    return n > 1e-12 ? mul(a, 1.0 / n) : a;
}

} // namespace

struct GazeRoomViewW::Impl {
    GazeFusionResult result;
    int64_t positionMs = 0;

    // Top-down basis: "up" is the calibrated plane's normal (towards the
    // cameras) when there is one, else the reference camera's up (-y, its
    // y points down); e1/e2 span the floor. Matches gaze/render.py TopDown,
    // so this panel and room_topdown.mp4 show the room the same way round.
    Vec3 e1 = {1, 0, 0}, e2 = {0, 0, 1};
    double minU = -1000, maxU = 1000, minV = -1000, maxV = 1000;

    [[nodiscard]] QPointF uv(const Vec3& p) const { return {dot(p, e1), dot(p, e2)}; }

    void recompute_basis_and_bounds() {
        Vec3 up = {0, -1, 0};
        if (result.plane_defined()) {
            up        = unit(result.plane_normal());
            Vec3 mean = {0, 0, 0};
            for (const auto& c : result.cameras()) {
                mean = add(mean, c.positionRoom);
            }
            if (!result.cameras().isEmpty()) {
                mean = mul(mean, 1.0 / result.cameras().size());
                if (dot(sub(mean, result.plane_point()), up) < 0) {
                    up = mul(up, -1.0);
                }
            }
        }
        const Vec3 refX = {1, 0, 0};
        e1              = unit(sub(refX, mul(up, dot(refX, up))));
        if (dot(e1, e1) < 0.25) {
            e1 = unit(cross(up, Vec3{0, 0, 1}));
        }
        // e1 to the right and e2 drawn downwards must look *down* the up
        // axis (e1 x e2 = -up); cross(up, e1) would show the room from
        // below, mirrored.
        e2 = cross(e1, up);

        minU = minV = std::numeric_limits<double>::max();
        maxU = maxV   = std::numeric_limits<double>::lowest();
        auto consider = [&](const Vec3& p) {
            const QPointF q = uv(p);
            minU            = std::min(minU, q.x());
            maxU            = std::max(maxU, q.x());
            minV            = std::min(minV, q.y());
            maxV            = std::max(maxV, q.y());
        };
        for (const auto& cam : result.cameras()) {
            consider(cam.positionRoom);
        }
        for (const auto& r : result.regions()) {
            consider(r.centre);
        }
        for (const auto& frame : result.frames()) {
            for (const auto& s : frame.subjects) {
                consider(s.origin);
            }
        }
        if (minU > maxU) {
            minU = minV = -1000;
            maxU = maxV = 1000;
        }
        const double pad = std::max({(maxU - minU) * 0.15, (maxV - minV) * 0.15, 400.0});
        minU -= pad;
        maxU += pad;
        minV -= pad;
        maxV += pad;
    }

    // One uniform scale for both axes: distances and angles on screen are
    // true to the room.
    [[nodiscard]] QPointF to_widget(const Vec3& p, const QRectF& area) const {
        const double scale = std::min(area.width() / std::max(maxU - minU, 1.0),
                                      area.height() / std::max(maxV - minV, 1.0));
        const double offX  = area.left() + (area.width() - (maxU - minU) * scale) / 2.0;
        const double offY  = area.top() + (area.height() - (maxV - minV) * scale) / 2.0;
        const QPointF q    = uv(p);
        return {offX + (q.x() - minU) * scale, offY + (q.y() - minV) * scale};
    }

    [[nodiscard]] QColor subject_color(const QString& id) const {
        const int i = std::max(0, static_cast<int>(result.subject_ids().indexOf(id)));
        return kSubjectColors[i % 6];
    }
};

GazeRoomViewW::GazeRoomViewW(QWidget* parent) : QWidget(parent), d(std::make_unique<Impl>()) {
    setMinimumSize(240, 180);
}

GazeRoomViewW::~GazeRoomViewW() = default;

void GazeRoomViewW::set_result(const GazeFusionResult& result) {
    d->result = result;
    d->recompute_basis_and_bounds();
    update();
}

void GazeRoomViewW::set_position_ms(int64_t positionMs) {
    d->positionMs = positionMs;
    update();
}

void GazeRoomViewW::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor("#0a0a1a"));

    if (!d->result.is_valid()) {
        painter.setPen(QColor("#404060"));
        painter.drawText(rect(), Qt::AlignCenter, "No gaze fusion result loaded.");
        return;
    }

    const QRectF area = QRectF(rect()).adjusted(12, 24, -12, -22);
    painter.setPen(QColor("#7070a0"));
    painter.drawText(rect().adjusted(8, 4, -8, -4), Qt::AlignTop | Qt::AlignLeft,
                     "Room from above");

    // Named regions, as their outline.
    for (const auto& r : d->result.regions()) {
        const Vec3 u = mul(unit(r.uAxis), r.width / 2.0);
        const Vec3 v = mul(unit(cross(r.normal, r.uAxis)), r.height / 2.0);
        const QPolygonF poly{d->to_widget(sub(sub(r.centre, u), v), area),
                             d->to_widget(sub(add(r.centre, u), v), area),
                             d->to_widget(add(add(r.centre, u), v), area),
                             d->to_widget(add(sub(r.centre, u), v), area)};
        painter.setPen(QPen(QColor("#9090b0"), 1.5));
        painter.setBrush(QColor(120, 120, 170, 40));
        painter.drawPolygon(poly);
        painter.setPen(QColor("#b0b0d0"));
        painter.drawText(poly.boundingRect().center() + QPointF(-12, 4), r.name);
    }

    // Cameras.
    for (const auto& cam : d->result.cameras()) {
        const QPointF p = d->to_widget(cam.positionRoom, area);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#a0a0b8"));
        painter.drawEllipse(p, 4, 4);
        painter.setPen(QColor("#a0a0b8"));
        painter.drawText(p + QPointF(6, -6), camera_short_label(cam.index));
    }

    const GazeFusionFrame* frame = d->result.frame_at_position_ms(d->positionMs);
    QStringList lines;
    if (frame) {
        for (const auto& s : frame->subjects) {
            const QColor color = d->subject_color(s.id);
            const QPointF o    = d->to_widget(s.origin, area);
            painter.setPen(QPen(color, 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(o, 9, 9);
            painter.drawText(o + QPointF(11, 4), s.name);
            if (s.hasDirection) {
                const Vec3 tip   = s.hasPoint ? s.point : add(s.origin, mul(s.direction, 800.0));
                const QPointF tp = d->to_widget(tip, area);
                painter.setPen(QPen(color, s.mutual ? 3 : 2));
                painter.drawLine(o, tp);
                painter.setPen(Qt::NoPen);
                painter.setBrush(color);
                painter.drawEllipse(tp, 4, 4);
            }
            QString target = s.targetType == QLatin1String("subject") ? s.targetLabel
                             : (s.targetLabel.isEmpty() || s.targetLabel == QLatin1String("none"))
                                 ? QStringLiteral("nothing recognised")
                                 : s.targetLabel;
            lines << QString("%1 → %2%3").arg(s.name, target, s.mutual ? "  (mutual)" : "");
        }
    }
    painter.setPen(QColor("#9090c0"));
    const QString info =
        frame ? (lines.isEmpty() ? QString("tick %1  ·  nobody seen").arg(frame->tick)
                                 : lines.join("   "))
              : QString("no data at this position");
    painter.drawText(rect().adjusted(8, 4, -8, -4), Qt::AlignBottom | Qt::AlignLeft, info);
}

} // namespace mosaic
