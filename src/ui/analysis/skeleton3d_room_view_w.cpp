#include "ui/analysis/skeleton3d_room_view_w.hpp"
#include "ui/analysis/subject_colors.hpp"
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QPainter>
#include <QVector3D>
#include <QVector4D>
#include <QWheelEvent>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>

namespace mosaic {

namespace {

const QColor kCameraColors[] = {
    QColor("#44cc44"), QColor("#4488ff"), QColor("#ffaa44"),
    QColor("#ff4488"), QColor("#44cccc"), QColor("#cc44cc"),
};

QVector3D to_qvec3(const Skeleton3DVec3& p) {
    return QVector3D(static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]));
}

} // namespace

struct Skeleton3DRoomViewW::Impl {
    Skeleton3DResult result;
    int64_t          positionMs = 0;

    // Orbit-camera spherical coordinates around `target`, room Z-up (same
    // "XY is the floor plane, Z is height" convention GazeRoomViewW's
    // top-down view already establishes for this codebase's room frame).
    double    yawDeg     = 35.0;
    double    pitchDeg   = 25.0;
    double    distanceMm = 3000.0;
    QVector3D target{0.0f, 0.0f, 0.0f};

    // Reset-view defaults, recomputed by recompute_bounds().
    double    defaultYawDeg     = 35.0;
    double    defaultPitchDeg   = 25.0;
    double    defaultDistanceMm = 3000.0;
    QVector3D defaultTarget{0.0f, 0.0f, 0.0f};

    bool   dragging = false;
    QPoint lastMousePos;

    void recompute_bounds() {
        QVector3D minP(std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max());
        QVector3D maxP(std::numeric_limits<float>::lowest(),
                        std::numeric_limits<float>::lowest(),
                        std::numeric_limits<float>::lowest());
        bool any = false;

        auto consider = [&](const Skeleton3DVec3& p) {
            const QVector3D v = to_qvec3(p);
            minP.setX(std::min(minP.x(), v.x())); maxP.setX(std::max(maxP.x(), v.x()));
            minP.setY(std::min(minP.y(), v.y())); maxP.setY(std::max(maxP.y(), v.y()));
            minP.setZ(std::min(minP.z(), v.z())); maxP.setZ(std::max(maxP.z(), v.z()));
            any = true;
        };

        for (const auto& cam : result.cameras()) { consider(cam.positionRoom); }
        for (const auto& frame : result.frames()) {
            for (const auto& person : frame.people) {
                for (const auto& kp : person.keypoints) {
                    if (kp.valid) { consider(kp.positionRoom); }
                }
            }
        }

        if (!any) {
            minP = QVector3D(-1000.0f, -1000.0f, -1000.0f);
            maxP = QVector3D(1000.0f, 1000.0f, 1000.0f);
        }

        target = (minP + maxP) * 0.5f;
        const float extent = std::max((maxP - minP).length(), 200.0f);
        distanceMm = std::max(static_cast<double>(extent) * 1.3, 500.0);

        defaultTarget     = target;
        defaultDistanceMm = distanceMm;
        yawDeg   = defaultYawDeg;
        pitchDeg = defaultPitchDeg;
    }

    [[nodiscard]] QVector3D eye_position() const {
        const double yawRad   = qDegreesToRadians(yawDeg);
        const double pitchRad = qDegreesToRadians(pitchDeg);
        const double cp = std::cos(pitchRad);
        const QVector3D dir(static_cast<float>(cp * std::cos(yawRad)),
                             static_cast<float>(cp * std::sin(yawRad)),
                             static_cast<float>(std::sin(pitchRad)));
        return target + dir * static_cast<float>(distanceMm);
    }

    [[nodiscard]] const Skeleton3DFrame* current_frame() const {
        if (result.frames().isEmpty()) { return nullptr; }
        const int64_t ts = result.frames().first().timestampNs + positionMs * 1000000LL;
        return result.nearest_frame(ts);
    }
};

Skeleton3DRoomViewW::Skeleton3DRoomViewW(QWidget* parent)
    : QWidget(parent), d(std::make_unique<Impl>()) {
    setMinimumSize(240, 180);
    setFocusPolicy(Qt::ClickFocus);
}

Skeleton3DRoomViewW::~Skeleton3DRoomViewW() = default;

void Skeleton3DRoomViewW::set_result(const Skeleton3DResult& result) {
    d->result = result;
    d->recompute_bounds();
    update();
}

void Skeleton3DRoomViewW::set_position_ms(int64_t positionMs) {
    d->positionMs = positionMs;
    update();
}

// ── Interaction ──────────────────────────────────────────────────────────

void Skeleton3DRoomViewW::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        d->dragging     = true;
        d->lastMousePos = event->pos();
    }
}

void Skeleton3DRoomViewW::mouseMoveEvent(QMouseEvent* event) {
    if (!d->dragging) { return; }
    const QPoint delta = event->pos() - d->lastMousePos;
    d->lastMousePos = event->pos();
    d->yawDeg += delta.x() * 0.4;
    d->pitchDeg = std::clamp(d->pitchDeg + delta.y() * 0.4, -85.0, 85.0);
    update();
}

void Skeleton3DRoomViewW::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) { d->dragging = false; }
}

void Skeleton3DRoomViewW::wheelEvent(QWheelEvent* event) {
    const double factor = std::pow(0.999, event->angleDelta().y());
    d->distanceMm = std::clamp(d->distanceMm * factor, 100.0, 500000.0);
    update();
}

void Skeleton3DRoomViewW::mouseDoubleClickEvent(QMouseEvent*) {
    d->yawDeg     = d->defaultYawDeg;
    d->pitchDeg   = d->defaultPitchDeg;
    d->distanceMm = d->defaultDistanceMm;
    d->target     = d->defaultTarget;
    update();
}

// ── Rendering ────────────────────────────────────────────────────────────

void Skeleton3DRoomViewW::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor("#0a0a1a"));

    if (!d->result.is_valid()) {
        painter.setPen(QColor("#404060"));
        painter.drawText(rect(), Qt::AlignCenter,
                          "No 3D pose result loaded.\nDrag to rotate, scroll to zoom, "
                          "double-click to reset.");
        return;
    }

    const QRectF viewport = QRectF(rect()).adjusted(12, 12, -12, -12);
    if (viewport.width() < 1.0 || viewport.height() < 1.0) { return; }

    const QVector3D eye = d->eye_position();
    const QVector3D up(0.0f, 0.0f, 1.0f);   // room Z-up

    QMatrix4x4 view;
    view.lookAt(eye, d->target, up);

    QMatrix4x4 proj;
    const float aspect = static_cast<float>(viewport.width() / viewport.height());
    const float farPlane = static_cast<float>(std::max(d->distanceMm * 10.0, 20000.0));
    proj.perspective(50.0f, aspect, 1.0f, farPlane);

    const QMatrix4x4 viewProj = proj * view;

    // Returns nullopt if the point is behind the eye (would otherwise
    // project to a nonsensical flipped screen position).
    const auto project = [&](const QVector3D& worldPt) -> std::optional<QPointF> {
        const QVector4D clip = viewProj * QVector4D(worldPt, 1.0f);
        if (clip.w() <= 0.01f) { return std::nullopt; }
        const float ndcX = clip.x() / clip.w();
        const float ndcY = clip.y() / clip.w();
        return QPointF(viewport.left() + (ndcX * 0.5 + 0.5) * viewport.width(),
                        viewport.top() + (1.0 - (ndcY * 0.5 + 0.5)) * viewport.height());
    };

    // Eye-space depth (positive, larger = farther) for back-to-front
    // painter's-algorithm sorting — QMatrix4x4::lookAt() follows the usual
    // OpenGL convention of looking down -Z in view space.
    const auto eye_depth = [&](const QVector3D& worldPt) -> float {
        return -(view * QVector4D(worldPt, 1.0f)).z();
    };

    struct DrawOp {
        float depth;
        std::function<void(QPainter&)> paint;
    };
    QVector<DrawOp> ops;

    // Floor grid, at the lowest Z among cameras+valid keypoints (the
    // auto-fit bounds' own lower extent, recovered from target/distance
    // isn't tracked separately, so approximate the floor as target.z()
    // minus half the current framing distance — good enough for a
    // schematic spatial-reference grid, not a claimed-exact floor plane).
    const float floorZ = d->target.z() - static_cast<float>(d->distanceMm) * 0.4f;
    const float halfExtent = static_cast<float>(d->distanceMm) * 0.8f;
    const float step = std::max(halfExtent / 6.0f, 100.0f);
    for (float g = -halfExtent; g <= halfExtent + 1.0f; g += step) {
        const QVector3D a1(d->target.x() + g, d->target.y() - halfExtent, floorZ);
        const QVector3D a2(d->target.x() + g, d->target.y() + halfExtent, floorZ);
        const QVector3D b1(d->target.x() - halfExtent, d->target.y() + g, floorZ);
        const QVector3D b2(d->target.x() + halfExtent, d->target.y() + g, floorZ);
        for (const auto& seg : {std::pair(a1, a2), std::pair(b1, b2)}) {
            const auto p1 = project(seg.first);
            const auto p2 = project(seg.second);
            if (!p1 || !p2) { continue; }
            const float depth = (eye_depth(seg.first) + eye_depth(seg.second)) * 0.5f;
            ops.append({depth, [p1, p2](QPainter& p) {
                p.setPen(QPen(QColor(60, 60, 90, 90), 1));
                p.drawLine(*p1, *p2);
            }});
        }
    }

    // Cameras.
    for (const auto& cam : d->result.cameras()) {
        const QVector3D worldPt = to_qvec3(cam.positionRoom);
        const auto pt = project(worldPt);
        if (!pt) { continue; }
        const QColor color = kCameraColors[((cam.index % 6) + 6) % 6];
        const int camIndex = cam.index;
        ops.append({eye_depth(worldPt), [pt, color, camIndex](QPainter& p) {
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawEllipse(*pt, 6, 6);
            p.setPen(color);
            p.drawText(*pt + QPointF(8, -8), QString("cam%1").arg(camIndex));
        }});
    }

    // Current frame's tracked people.
    const Skeleton3DFrame* frame = d->current_frame();
    int totalPeople = 0;
    if (frame) {
        totalPeople = frame->people.size();
        const auto& edges = d->result.skeleton_edges();
        for (const auto& person : frame->people) {
            const QColor color = subject_color(person.trackId);

            for (const auto& edge : edges) {
                const int a = edge.first, b = edge.second;
                if (a < 0 || b < 0 || a >= person.keypoints.size() || b >= person.keypoints.size()) {
                    continue;
                }
                if (!person.keypoints[a].valid || !person.keypoints[b].valid) { continue; }
                const QVector3D wa = to_qvec3(person.keypoints[a].positionRoom);
                const QVector3D wb = to_qvec3(person.keypoints[b].positionRoom);
                const auto pa = project(wa);
                const auto pb = project(wb);
                if (!pa || !pb) { continue; }
                const float depth = (eye_depth(wa) + eye_depth(wb)) * 0.5f;
                ops.append({depth, [pa, pb, color](QPainter& p) {
                    p.setPen(QPen(color, 2));
                    p.drawLine(*pa, *pb);
                }});
            }

            int firstValid = -1;
            for (int i = 0; i < person.keypoints.size(); ++i) {
                if (!person.keypoints[i].valid) { continue; }
                if (firstValid < 0) { firstValid = i; }
                const QVector3D w = to_qvec3(person.keypoints[i].positionRoom);
                const auto pt = project(w);
                if (!pt) { continue; }
                ops.append({eye_depth(w), [pt, color](QPainter& p) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(color);
                    p.drawEllipse(*pt, 3, 3);
                }});
            }

            if (firstValid >= 0) {
                const QVector3D w = to_qvec3(person.keypoints[firstValid].positionRoom);
                const auto pt = project(w);
                if (pt) {
                    const int trackId = person.trackId;
                    ops.append({eye_depth(w), [pt, color, trackId](QPainter& p) {
                        p.setPen(color);
                        p.drawText(*pt + QPointF(6, -6), QString("track %1").arg(trackId));
                    }});
                }
            }
        }
    }

    // Painter's algorithm: farthest first.
    std::sort(ops.begin(), ops.end(),
              [](const DrawOp& a, const DrawOp& b) { return a.depth > b.depth; });
    for (const auto& op : ops) { op.paint(painter); }

    painter.setPen(QColor("#7070a0"));
    const QString info = frame
        ? QString("tick %1  ·  %2 person(s)").arg(frame->tick).arg(totalPeople)
        : QString("no data at this position");
    painter.drawText(rect().adjusted(6, 4, -6, -4), Qt::AlignBottom | Qt::AlignLeft, info);
}

} // namespace mosaic
