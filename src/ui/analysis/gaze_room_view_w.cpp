#include "ui/analysis/gaze_room_view_w.hpp"

#include <QPainter>
#include <algorithm>
#include <limits>

namespace mosaic {

namespace {
const QColor kCameraColors[] = {
    QColor("#44cc44"), QColor("#4488ff"), QColor("#ffaa44"),
    QColor("#ff4488"), QColor("#44cccc"), QColor("#cc44cc"),
};
} // namespace

struct GazeRoomViewW::Impl {
    GazeFusionResult result;
    int64_t positionMs = 0;

    // Auto-fit projection bounds, room-space mm (X/Y — top-down).
    double minX = -1000, maxX = 1000, minY = -1000, maxY = 1000;

    void recompute_bounds() {
        minX = minY = std::numeric_limits<double>::max();
        maxX = maxY = std::numeric_limits<double>::lowest();

        auto consider = [&](const Vec3& p) {
            minX = std::min(minX, p[0]);
            maxX = std::max(maxX, p[0]);
            minY = std::min(minY, p[1]);
            maxY = std::max(maxY, p[1]);
        };

        for (const auto& cam : result.cameras()) {
            consider(cam.positionRoom);
        }
        if (result.plane_defined()) {
            consider(result.plane_point());
        }
        for (const auto& frame : result.frames()) {
            if (frame.numCameras > 0) {
                consider(frame.fusedOriginRoom);
            }
            if (frame.hasTarget) {
                consider(frame.targetPointRoom);
            }
        }

        if (minX > maxX) { // nothing considered (empty/invalid result) — fall back
            minX = -1000;
            maxX = 1000;
            minY = -1000;
            maxY = 1000;
        }
        const double padX = std::max((maxX - minX) * 0.15, 100.0);
        const double padY = std::max((maxY - minY) * 0.15, 100.0);
        minX -= padX;
        maxX += padX;
        minY -= padY;
        maxY += padY;
    }

    // Room X/Y -> widget coordinates, independently scaled per axis to
    // fill `area` (not forced to a single uniform scale — this is a schematic
    // top-down diagram, not a metrically-accurate blueprint). Room Y is
    // flipped so it grows "up" on screen, matching a conventional top-down
    // view rather than Qt's own downward-growing Y.
    [[nodiscard]] QPointF to_widget(const Vec3& p, const QRectF& area) const {
        const double u = (p[0] - minX) / std::max(maxX - minX, 1e-6);
        const double v = (p[1] - minY) / std::max(maxY - minY, 1e-6);
        return QPointF(area.left() + u * area.width(), area.bottom() - v * area.height());
    }

    [[nodiscard]] const GazeFusionFrame* current_frame() const {
        if (result.frames().isEmpty()) {
            return nullptr;
        }
        const int64_t ts = result.frames().first().timestampNs + positionMs * 1000000LL;
        return result.nearest_frame(ts);
    }
};

GazeRoomViewW::GazeRoomViewW(QWidget* parent) : QWidget(parent), d(std::make_unique<Impl>()) {
    setMinimumSize(240, 180);
}

GazeRoomViewW::~GazeRoomViewW() = default;

void GazeRoomViewW::set_result(const GazeFusionResult& result) {
    d->result = result;
    d->recompute_bounds();
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

    const QRectF area = QRectF(rect()).adjusted(12, 12, -12, -12);

    // Reference plane — object points aren't known (only point+normal), so
    // draw a simple square footprint centered on the plane point as a
    // schematic stand-in for "the target surface is roughly here."
    if (d->result.plane_defined()) {
        const auto pp           = d->result.plane_point();
        const double halfExtent = std::max(d->maxX - d->minX, d->maxY - d->minY) * 0.15;
        const Vec3 corner1      = {pp[0] - halfExtent, pp[1] - halfExtent, pp[2]};
        const Vec3 corner2      = {pp[0] + halfExtent, pp[1] + halfExtent, pp[2]};
        painter.setPen(QPen(QColor("#335544"), 1));
        painter.setBrush(QColor(60, 110, 90, 60));
        painter.drawRect(
            QRectF(d->to_widget(corner1, area), d->to_widget(corner2, area)).normalized());
    }

    // Cameras.
    for (const auto& cam : d->result.cameras()) {
        const QPointF p    = d->to_widget(cam.positionRoom, area);
        const QColor color = kCameraColors[((cam.index % 6) + 6) % 6];
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(p, 5, 5);
        painter.setPen(color);
        painter.drawText(p + QPointF(6, -6), QString("cam%1").arg(cam.index));
    }

    // Current fused frame: per-camera rays (thin) + the fused ray (bold).
    const GazeFusionFrame* frame = d->current_frame();
    if (frame && frame->numCameras > 0) {
        for (const auto& cam : frame->perCamera) {
            const Vec3 tip = {cam.originRoom[0] + cam.directionRoom[0] * 500.0,
                              cam.originRoom[1] + cam.directionRoom[1] * 500.0,
                              cam.originRoom[2] + cam.directionRoom[2] * 500.0};
            painter.setPen(QPen(QColor(255, 255, 255, 70), 1, Qt::DashLine));
            painter.drawLine(d->to_widget(cam.originRoom, area), d->to_widget(tip, area));
        }

        const QPointF fusedOrigin = d->to_widget(frame->fusedOriginRoom, area);
        QPointF fusedTip;
        if (frame->hasTarget) {
            fusedTip = d->to_widget(frame->targetPointRoom, area);
        } else {
            const Vec3 tip = {frame->fusedOriginRoom[0] + frame->fusedDirectionRoom[0] * 800.0,
                              frame->fusedOriginRoom[1] + frame->fusedDirectionRoom[1] * 800.0,
                              frame->fusedOriginRoom[2] + frame->fusedDirectionRoom[2] * 800.0};
            fusedTip       = d->to_widget(tip, area);
        }
        painter.setPen(QPen(QColor("#ffdd44"), 2));
        painter.drawLine(fusedOrigin, fusedTip);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#ffdd44"));
        painter.drawEllipse(fusedTip, 4, 4);
    }

    painter.setPen(QColor("#7070a0"));
    const QString info =
        frame ? QString("tick %1  ·  %2 camera(s)%3")
                    .arg(frame->tick)
                    .arg(frame->numCameras)
                    .arg(frame->isTriangulated
                             ? QString("  ·  residual %1 mm").arg(frame->residualRmsMm, 0, 'f', 1)
                             : QString())
              : QString("no data at this position");
    painter.drawText(rect().adjusted(6, 4, -6, -4), Qt::AlignBottom | Qt::AlignLeft, info);
}

} // namespace mosaic
