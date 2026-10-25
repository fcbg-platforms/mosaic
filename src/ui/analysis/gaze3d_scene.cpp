#include "ui/analysis/gaze3d_scene.hpp"

#include <QQuaternion>
#include <algorithm>
#include <cmath>

#include "analysis/gaze_summary.hpp"
#include "analysis/room_view_math.hpp"
#include "ui/analysis/gaze_colors.hpp"

namespace mosaic {

using room3d::alpha;
using room3d::put;
using room3d::qv;

namespace {

constexpr double kTrailSeconds = 2.0;
constexpr double kTrailGapS    = 0.5;    // a trail never spans a longer gap
constexpr float kFreeRayMm     = 1500.0; // a gaze landing on nothing, drawn this long
constexpr float kPlaneSizeMm   = 2000.0; // the calibrated plane, drawn as a square this wide

QVector3D unit_or(const QVector3D& v, const QVector3D& fallback) {
    return v.length() > 1e-6f ? v.normalized() : fallback;
}

} // namespace

Gaze3DScene::Gaze3DScene(QObject* parent) : Room3DMeshSource(parent) {}

// As on the video: the annotated videos' and the live overlay's colour.
QColor Gaze3DScene::color_of(const QString& id) const { return gaze_subject_color(id); }

QVariantList Gaze3DScene::people() const {
    QVariantList out;
    for (const auto& [id, seconds] : seen_) {
        out << QVariantMap{{"id", id},
                           {"label", result_.subject_name(id)},
                           {"seconds", seconds},
                           {"color", color_of(id)},
                           {"shown", !hidden_.contains(id)}};
    }
    return out;
}

void Gaze3DScene::set_result(const GazeFusionResult& result,
                             std::optional<Room3DStage::Basis> room) {
    result_ = result;
    follow_.clear();
    hidden_.clear();
    seen_.clear();
    if (!result_.is_valid()) {
        stage_.clear_room();
        emit resultChanged();
        emit peopleChanged();
        rebuild_frame();
        return;
    }
    for (const auto& s : summarize_gaze(result_).subjects) {
        seen_.push_back({s.id, s.secondsSeen});
    }

    // Which way is up: the 3D Pose result's (the same room), else the
    // calibrated plane's normal, turned towards the cameras, else the
    // cameras'.
    Room3DStage::Basis basis;
    if (room) {
        basis = *room;
    } else {
        std::optional<QVector3D> up;
        if (result_.plane_defined()) {
            QVector3D n = unit_or(qv(result_.plane_normal()), QVector3D(0, -1, 0));
            float above = 0.0f;
            for (const auto& c : result_.cameras()) {
                above += QVector3D::dotProduct(qv(c.positionRoom) - qv(result_.plane_point()), n);
            }
            up = above < 0.0f ? -n : n;
            // A plane on a wall (a whiteboard) is not level: its normal is
            // up only within 45 degrees of the cameras' own up, when they
            // say which way they look.
            const bool camerasKnowUp =
                std::any_of(result_.cameras().cbegin(), result_.cameras().cend(),
                            [](const auto& c) { return c.hasRotation; });
            if (camerasKnowUp &&
                QVector3D::dotProduct(*up, qv(room_view::up_from_cameras(result_.cameras()))) <
                    std::cos(45.0 * M_PI / 180.0)) {
                up.reset();
            }
        }
        basis = Room3DStage::basis_of_cameras(result_.cameras(), up);
    }
    // Framed on where the people's heads were, and the regions.
    std::vector<Skeleton3DVec3> points;
    for (const auto& f : result_.frames()) {
        for (const auto& s : f.subjects) {
            points.push_back(s.origin);
        }
    }
    for (const auto& r : result_.regions()) {
        points.push_back(r.centre);
    }
    stage_.set_room(basis, result_.cameras(), points);
    emit resultChanged();
    emit peopleChanged();
    rebuild_frame();
}

void Gaze3DScene::set_position_ms(int64_t positionMs) {
    positionMs_ = positionMs;
    rebuild_frame();
}

void Gaze3DScene::set_shown(const QString& id, bool shown) {
    if (shown) {
        hidden_.remove(id);
    } else {
        hidden_.insert(id);
    }
    emit peopleChanged();
    rebuild_frame();
}

void Gaze3DScene::set_follow(const QString& id) {
    follow_ = id;
    emit peopleChanged();
    rebuild_frame();
}

void Gaze3DScene::set_trails(bool on) {
    trails_ = on;
    rebuild_frame();
}

// The tick on show: heads, rays, gaze points, the targets lit, the labels,
// the trails.
void Gaze3DScene::rebuild_frame() {
    heads_.clear();
    rays_.clear();
    points_.clear();
    regions_.clear();
    tags_.clear();
    plane_.clear();
    info_.clear();
    followFound_ = false;
    tick_        = -1;
    Mesh trails;

    const GazeFusionFrame* frame =
        result_.is_valid() ? result_.frame_at_position_ms(positionMs_) : nullptr;
    const QVector3D yAxis(0, 1, 0);
    QMap<QString, QColor> litBy; // "region/<name>" or "plane" -> who looks at it
    int shownCount = 0;
    if (frame) {
        tick_ = frame->tick;
        for (const auto& s : frame->subjects) {
            if (hidden_.contains(s.id)) {
                continue;
            }
            ++shownCount;
            const QColor col    = color_of(s.id);
            const QVector3D eye = stage_.to_scene(s.origin);
            QString looking     = s.hasDirection ? QString("looking at nothing")
                                                 : QString("no gaze (eyes not usable)");
            if (s.hasDirection && !s.targetType.isEmpty() && s.targetType != "none") {
                looking = "looking at " + s.targetLabel + (s.mutual ? " (mutual)" : "");
                if (s.targetType == "region" || s.targetType == "plane") {
                    litBy.insert(
                        s.targetType == "plane" ? QString("plane") : "region/" + s.targetLabel,
                        col);
                }
            }
            heads_ << QVariantMap{
                {"pos", eye},
                {"color", col},
                {"info", QString("%1\n%2\n%3 camera(s)%4\nx %5   y %6   z %7 m")
                             .arg(s.name, looking)
                             .arg(s.numCameras)
                             .arg(s.uncertaintyDeg >= 0
                                      ? QString("  ·  ±%1°").arg(s.uncertaintyDeg, 0, 'f', 1)
                                      : QString())
                             .arg(s.origin[0] / 1000.0, 0, 'f', 2)
                             .arg(s.origin[1] / 1000.0, 0, 'f', 2)
                             .arg(s.origin[2] / 1000.0, 0, 'f', 2)}};
            // The gaze: from the eyes to where it lands, else a fixed length.
            if (s.hasDirection) {
                const QVector3D end =
                    s.hasPoint
                        ? stage_.to_scene(s.point)
                        : eye + unit_or(stage_.dir_to_scene(qv(s.direction)), yAxis) * kFreeRayMm;
                const QVector3D d = end - eye;
                if (d.length() > 1.0f) {
                    rays_ << QVariantMap{{"pos", (eye + end) / 2.0f},
                                         {"rot", QQuaternion::rotationTo(yAxis, d.normalized())},
                                         {"len", d.length()},
                                         {"color", col},
                                         {"mutual", s.mutual}};
                }
                if (s.hasPoint) {
                    points_ << QVariantMap{{"pos", end}, {"color", col}};
                }
            }
            tags_ << QVariantMap{
                {"pos", eye + QVector3D(0, 230, 0)},
                {"text",
                 s.hasDirection && !s.targetType.isEmpty() && s.targetType != "none"
                     ? QString("%1 %2 %3")
                           .arg(s.name, s.mutual ? QChar(0x2194) : QChar(0x2192), s.targetLabel)
                     : s.name},
                {"color", col}};
            if (s.id == follow_) {
                followTarget_ = QVector3D(eye.x(), 1000.0f, eye.z());
                followFound_  = true;
            }

            // Trails: the gaze points over the last seconds, fading; never
            // across a gap.
            if (trails_) {
                const auto& frames = result_.frames();
                const int now      = static_cast<int>(frame - frames.constData());
                std::optional<QVector3D> next =
                    s.hasPoint ? std::optional<QVector3D>(stage_.to_scene(s.point)) : std::nullopt;
                double nextAge = 0.0;
                for (int k = now - 1; k >= 0; --k) {
                    const double age = (frame->timestampNs - frames[k].timestampNs) / 1e9;
                    if (age > kTrailSeconds) {
                        break;
                    }
                    std::optional<QVector3D> here;
                    if (const auto* q = frames[k].subject(s.id); q && q->hasPoint) {
                        here = stage_.to_scene(q->point);
                    }
                    if (here && next && age - nextAge <= kTrailGapS) {
                        put(trails.vertices, *next,
                            alpha(col, 0.9 * (1.0 - nextAge / kTrailSeconds)));
                        put(trails.vertices, *here, alpha(col, 0.9 * (1.0 - age / kTrailSeconds)));
                    }
                    if (here) {
                        next    = here;
                        nextAge = age;
                    }
                }
            }
        }
        info_ = QString("tick %1  ·  %2 person(s) shown").arg(frame->tick).arg(shownCount);
    } else if (result_.is_valid()) {
        info_ = "no data at this position";
    }

    // The named regions, and the calibrated plane: lit in the colour of
    // whoever looks at them now.
    if (result_.is_valid()) {
        for (const auto& r : result_.regions()) {
            const QVector3D n = unit_or(stage_.dir_to_scene(qv(r.normal)), yAxis);
            QVector3D u       = stage_.dir_to_scene(qv(r.uAxis));
            u                 = unit_or(u - n * QVector3D::dotProduct(u, n), QVector3D(1, 0, 0));
            const QVector3D v = QVector3D::crossProduct(n, u);
            const bool lit    = litBy.contains("region/" + r.name);
            regions_ << QVariantMap{
                {"pos", stage_.to_scene(r.centre)},
                {"rot", QQuaternion::fromAxes(u, v, n)},
                {"width", r.width},
                {"height", r.height},
                {"name", r.name},
                {"lit", lit},
                {"color", lit ? litBy.value("region/" + r.name) : QColor("#8a93c8")}};
        }
        if (result_.plane_defined()) {
            const QVector3D n = unit_or(stage_.dir_to_scene(qv(result_.plane_normal())), yAxis);
            // Any direction in the plane, then the other at right angles.
            QVector3D u = QVector3D::crossProduct(n, QVector3D(0, 0, 1));
            u           = unit_or(u, QVector3D::crossProduct(n, QVector3D(1, 0, 0)).normalized());
            const QVector3D v = QVector3D::crossProduct(n, u);
            const bool lit    = litBy.contains("plane");
            plane_            = QVariantMap{{"visible", true},
                                            {"pos", stage_.to_scene(result_.plane_point())},
                                            {"rot", QQuaternion::fromAxes(u, v, n)},
                                            {"size", kPlaneSizeMm},
                                            {"lit", lit},
                                            {"color", lit ? litBy.value("plane") : QColor("#8a93c8")}};
        }
    }
    set_mesh("trails", std::move(trails));
    emit frameChanged();
}

} // namespace mosaic
