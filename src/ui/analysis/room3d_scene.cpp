#include "ui/analysis/room3d_scene.hpp"

#include <QQuaternion>
#include <algorithm>
#include <cmath>
#include <optional>

#include "analysis/room_view_math.hpp"
#include "ui/analysis/subject_colors.hpp"
#include "video/camera_label.hpp"

namespace mosaic {

namespace {

const QColor kCameraColors[] = {
    QColor("#44cc44"), QColor("#4488ff"), QColor("#ffaa44"),
    QColor("#ff4488"), QColor("#44cccc"), QColor("#cc44cc"),
};
constexpr double kTrailSeconds = 2.0;
constexpr double kTrailGapS    = 0.25; // a trail never spans a longer gap
constexpr float kGridStepMm    = 500.0f;

QVector3D qv(const Skeleton3DVec3& p) {
    return QVector3D(static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]));
}

QVector3D column(const std::array<double, 9>& m, int j) {
    return QVector3D(static_cast<float>(m[j]), static_cast<float>(m[3 + j]),
                     static_cast<float>(m[6 + j]));
}

QColor camera_color(int index) { return kCameraColors[((index % 6) + 6) % 6]; }

// Person N (1, 2, ...) in the Pose plugin's colour order (subject_colors.hpp).
QColor person_color(int person) { return subject_color(person - 1); }

void put(std::vector<float>& v, const QVector3D& p, const QColor& c) {
    v.insert(v.end(),
             {p.x(), p.y(), p.z(), static_cast<float>(c.redF()), static_cast<float>(c.greenF()),
              static_cast<float>(c.blueF()), static_cast<float>(c.alphaF())});
}

QColor alpha(QColor c, double a) {
    c.setAlphaF(static_cast<float>(std::clamp(a, 0.0, 1.0)));
    return c;
}

QString pretty(const QString& name) {
    QString s = name;
    s.replace('_', ' ');
    return s;
}

} // namespace

Room3DScene::Room3DScene(QObject* parent) : QObject(parent) {}

QVector3D Room3DScene::to_scene(const Skeleton3DVec3& room) const {
    const QVector3D v = qv(room);
    return {QVector3D::dotProduct(v, right_), QVector3D::dotProduct(v, up_) - floorH_,
            -QVector3D::dotProduct(v, fwd_)};
}

QVector3D Room3DScene::dir_to_scene(const QVector3D& d) const {
    return {QVector3D::dotProduct(d, right_), QVector3D::dotProduct(d, up_),
            -QVector3D::dotProduct(d, fwd_)};
}

const Room3DScene::Mesh& Room3DScene::mesh(const QString& part) const {
    static const Mesh kEmpty;
    const auto it = meshes_.constFind(part);
    return it == meshes_.cend() ? kEmpty : it.value();
}

QVariantList Room3DScene::axes() const {
    return {
        QVariantMap{{"name", "x"}, {"dir", dir_to_scene({1, 0, 0})}, {"color", QColor("#ff6666")}},
        QVariantMap{{"name", "y"}, {"dir", dir_to_scene({0, 1, 0})}, {"color", QColor("#66dd66")}},
        QVariantMap{{"name", "z"}, {"dir", dir_to_scene({0, 0, 1})}, {"color", QColor("#6699ff")}}};
}

QVariantList Room3DScene::people() const {
    QVariantList out;
    for (auto it = seconds_.cbegin(); it != seconds_.cend(); ++it) {
        out << QVariantMap{{"id", it.key()},
                           {"label", QString("Person %1").arg(it.key())},
                           {"seconds", it.value()},
                           {"color", person_color(it.key())},
                           {"shown", !hidden_.contains(it.key())},
                           {"brief", it.value() < kBriefPersonS}};
    }
    return out;
}

void Room3DScene::set_result(const Skeleton3DResult& result) {
    result_ = result;
    follow_ = -1;
    rebuild_static();
    // People seen for under a second start hidden (see lasting_people());
    // the People panel can show them.
    hidden_.clear();
    const QList<int> lasting = result_.lasting_people();
    for (auto it = seconds_.cbegin(); it != seconds_.cend(); ++it) {
        if (!lasting.contains(it.key())) {
            hidden_.insert(it.key());
        }
    }
    emit resultChanged();
    emit peopleChanged();
    rebuild_frame();
}

void Room3DScene::set_position_ms(int64_t positionMs) {
    positionMs_ = positionMs;
    rebuild_frame();
}

void Room3DScene::set_show_smoothed(bool smoothed) {
    smoothed_ = smoothed;
    rebuild_frame();
}

void Room3DScene::set_dyad(int personA, int personB) {
    dyadA_ = personA;
    dyadB_ = personB;
    rebuild_frame();
}

void Room3DScene::set_shown(int person, bool shown) {
    if (shown) {
        hidden_.remove(person);
    } else {
        hidden_.insert(person);
    }
    emit peopleChanged();
    rebuild_frame();
}

void Room3DScene::set_follow(int person) {
    follow_ = person;
    emit peopleChanged();
    rebuild_frame();
}

void Room3DScene::set_trails(bool on) {
    trails_ = on;
    rebuild_frame();
}

// What does not change during playback: the room's orientation, the
// framing, the cameras, the floor grid.
void Room3DScene::rebuild_static() {
    cameras_.clear();
    seconds_.clear();
    meshes_.clear();
    const auto& names = result_.keypoint_names();
    auto idx          = [&](const char* n) { return static_cast<int>(names.indexOf(n)); };
    lHip_ = idx("left_hip"), rHip_ = idx("right_hip");
    lSh_ = idx("left_shoulder"), rSh_ = idx("right_shoulder");
    nose_ = idx("nose"), lEar_ = idx("left_ear"), rEar_ = idx("right_ear");
    headIdx_.clear();
    for (const char* n : {"nose", "left_eye", "right_eye", "left_ear", "right_ear"}) {
        if (idx(n) >= 0) {
            headIdx_ << idx(n);
        }
    }
    if (!result_.is_valid()) {
        return;
    }

    const auto up  = room_view::estimate_up(result_);
    const auto fwd = room_view::floor_forward(up);
    up_            = qv(up);
    fwd_           = qv(fwd);
    right_         = QVector3D::crossProduct(fwd_, up_).normalized();
    floorH_        = static_cast<float>(room_view::floor_height(result_, up));

    frameCamera_ = "the reference camera";
    for (const auto& c : result_.cameras()) {
        if (room_view::norm(c.positionRoom) < 1.0 &&
            (!c.hasRotation ||
             std::abs(c.rotationRoom[0] + c.rotationRoom[4] + c.rotationRoom[8] - 3.0) < 1e-6)) {
            frameCamera_ = camera_label(c.index);
        }
    }

    // Where the people were (their mean, at 1 m), how far the cameras and
    // most of the people reach, and how long each person was in 3D.
    const auto& frames = result_.frames();
    seconds_           = result_.seconds_by_person();
    QVector3D sum;
    std::vector<QVector3D> pts;
    for (const auto& f : frames) {
        for (const auto& p : f.people) {
            for (const auto& kp : p.keypoints) {
                if (kp.valid) {
                    pts.push_back(to_scene(kp.positionRoom));
                    sum += pts.back();
                }
            }
        }
    }
    if (pts.empty()) {
        for (const auto& c : result_.cameras()) {
            pts.push_back(to_scene(c.positionRoom));
            sum += pts.back();
        }
    }
    home_ = pts.empty() ? QVector3D(0, 1000, 0) : sum / static_cast<float>(pts.size());
    home_.setY(1000.0f);
    auto flat = [this](const QVector3D& p) {
        return QVector2D(p.x() - home_.x(), p.z() - home_.z()).length();
    };
    float r = 1500.0f;
    for (const auto& c : result_.cameras()) {
        r = std::max(r, flat(to_scene(c.positionRoom)));
    }
    if (!pts.empty()) {
        std::sort(pts.begin(), pts.end(),
                  [&](const QVector3D& a, const QVector3D& b) { return flat(a) < flat(b); });
        r = std::max(r, flat(pts[pts.size() * 95 / 100])); // strays do not count
    }
    extent_    = r;
    floorSize_ = 2.0f * (std::ceil((r + 1000.0f) / 1000.0f) * 1000.0f);

    // The floor grid: every 0.5 m, brighter each metre, on whole metres.
    Mesh grid;
    const float half = floorSize_ / 2.0f;
    const float cx   = floor_centre().x();
    const float cz   = floor_centre().z();
    for (float g = -half; g <= half + 1.0f; g += kGridStepMm) {
        const bool metre = std::fmod(std::abs(g), 1000.0f) < 1.0f;
        // Dark colours rather than transparency (lines ignore alpha); they
        // are linear light, brightened on the way to the screen.
        const QColor c = metre ? QColor(34, 38, 62) : QColor(18, 20, 34);
        put(grid.vertices, {cx + g, 1.0f, cz - half}, c);
        put(grid.vertices, {cx + g, 1.0f, cz + half}, c);
        put(grid.vertices, {cx - half, 1.0f, cz + g}, c);
        put(grid.vertices, {cx + half, 1.0f, cz + g}, c);
    }
    meshes_["grid"] = grid;

    // The cameras: their bodies are posed in the page; what each sees is a
    // pyramid 0.45 m deep, outlined, with a translucent far face.
    Mesh frusta, faces;
    faces.triangles = true;
    for (const auto& c : result_.cameras()) {
        const QVector3D o = to_scene(c.positionRoom);
        QVariantMap cam{{"pos", o},
                        {"color", camera_color(c.index)},
                        {"label", camera_label(c.index)},
                        {"hasRotation", c.hasRotation}};
        if (c.hasRotation) {
            const QVector3D x = dir_to_scene(column(c.rotationRoom, 0));
            const QVector3D y = dir_to_scene(column(c.rotationRoom, 1));
            const QVector3D z = dir_to_scene(column(c.rotationRoom, 2));
            // A node looks down its -z with +y up: the camera's look is z,
            // its image-up is -y.
            cam["rot"]           = QQuaternion::fromAxes(x, -y, -z);
            const float depth    = 450.0f;
            const float tx       = depth * static_cast<float>(std::tan(c.fovDeg[0] * M_PI / 360.0));
            const float ty       = depth * static_cast<float>(std::tan(c.fovDeg[1] * M_PI / 360.0));
            const QVector3D k[4] = {
                o + z * depth - x * tx - y * ty, o + z * depth + x * tx - y * ty,
                o + z * depth + x * tx + y * ty, o + z * depth - x * tx + y * ty};
            const QColor col = camera_color(c.index);
            for (int i = 0; i < 4; ++i) {
                put(frusta.vertices, o, alpha(col, 0.9));
                put(frusta.vertices, k[i], alpha(col, 0.9));
                put(frusta.vertices, k[i], alpha(col, 0.9));
                put(frusta.vertices, k[(i + 1) % 4], alpha(col, 0.9));
            }
            for (const int i : {0, 1, 2, 0, 2, 3}) {
                put(faces.vertices, k[i], alpha(col, 0.18));
            }
        }
        cameras_ << cam;
    }
    meshes_["frusta"]       = frusta;
    meshes_["frustumFaces"] = faces;
}

// The frame on show: people, trails, the two-person line, the labels.
void Room3DScene::rebuild_frame() {
    bones_.clear();
    joints_.clear();
    heads_.clear();
    tags_.clear();
    dyad_.clear();
    Mesh trails, torsos;
    torsos.triangles = true;
    info_.clear();
    followFound_ = false;

    const auto& frames           = result_.frames();
    const Skeleton3DFrame* frame = nullptr;
    if (result_.is_valid() && !frames.isEmpty()) {
        frame = result_.nearest_frame(frames.first().timestampNs + positionMs_ * 1000000LL);
    }
    if (!frame) {
        meshes_["trails"] = trails;
        meshes_["torsos"] = torsos;
        emit frameChanged();
        return;
    }
    const auto& names = result_.keypoint_names();
    auto pos          = [&](const Skeleton3DPerson& p, int i) -> std::optional<QVector3D> {
        if (i < 0 || i >= p.keypoints.size() || !p.keypoints[i].valid) {
            return std::nullopt;
        }
        const auto& kp = p.keypoints[i];
        return to_scene(smoothed_ ? kp.positionRoomSmoothed : kp.positionRoom);
    };
    auto hips = [&](const Skeleton3DPerson& p) -> std::optional<QVector3D> {
        const auto a = pos(p, lHip_), b = pos(p, rHip_);
        return a && b ? std::optional<QVector3D>((*a + *b) / 2.0f) : std::nullopt;
    };
    const QVector3D yAxis(0, 1, 0);

    int shownCount = 0;
    for (const auto& p : frame->people) {
        if (hidden_.contains(p.trackId)) {
            continue;
        }
        ++shownCount;
        const QColor col  = person_color(p.trackId);
        const QString who = QString("Person %1").arg(p.trackId);

        for (const auto& e : result_.skeleton_edges()) {
            if (headIdx_.contains(e.first) && headIdx_.contains(e.second)) {
                continue; // the face is drawn as a head
            }
            const auto a = pos(p, e.first), b = pos(p, e.second);
            if (!a || !b || (*b - *a).length() < 1.0f) {
                continue;
            }
            const QVector3D d = *b - *a;
            bones_ << QVariantMap{{"pos", (*a + *b) / 2.0f},
                                  {"rot", QQuaternion::rotationTo(yAxis, d.normalized())},
                                  {"len", d.length()},
                                  {"color", col}};
        }
        QVector3D headSum, top;
        int headN  = 0;
        float topY = -1e30f;
        for (int i = 0; i < p.keypoints.size(); ++i) {
            const auto q = pos(p, i);
            if (!q) {
                continue;
            }
            if (q->y() > topY) {
                topY = q->y();
                top  = *q;
            }
            if (headIdx_.contains(i)) {
                headSum += *q;
                ++headN;
                continue;
            }
            // The room frame, as exported; the position drawn (smoothed when
            // Show smoothed is on).
            const auto& r =
                smoothed_ ? p.keypoints[i].positionRoomSmoothed : p.keypoints[i].positionRoom;
            joints_ << QVariantMap{
                {"pos", *q},
                {"color", col},
                {"info", QString("%1 · %2\nx %3   y %4   z %5 m")
                             .arg(who, pretty(i < names.size() ? names[i] : QString::number(i)))
                             .arg(r[0] / 1000.0, 0, 'f', 2)
                             .arg(r[1] / 1000.0, 0, 'f', 2)
                             .arg(r[2] / 1000.0, 0, 'f', 2)}};
        }
        if (headN) {
            const QVector3D head = headSum / static_cast<float>(headN);
            QVariantMap h{{"pos", head}, {"color", col}, {"hasFacing", false}};
            // Facing: from between the ears (else the head's centre) to the
            // nose, laid flat on the floor.
            const auto nose = pos(p, nose_);
            const auto le = pos(p, lEar_), re = pos(p, rEar_);
            const QVector3D back = le && re ? (*le + *re) / 2.0f : head;
            if (nose) {
                QVector3D d = *nose - back;
                d.setY(0);
                if (d.length() > 20.0f) {
                    d.normalize();
                    h["hasFacing"] = true;
                    h["facingPos"] = head + d * 150.0f;
                    h["facingRot"] = QQuaternion::rotationTo(yAxis, d);
                }
            }
            heads_ << h;
        }
        if (topY > -1e29f) {
            tags_ << QVariantMap{
                {"pos", top + QVector3D(0, 260, 0)}, {"text", who}, {"color", col}};
        }

        // The torso, filled between the shoulders and the hips.
        const auto ls = pos(p, lSh_), rs = pos(p, rSh_), lh = pos(p, lHip_), rh = pos(p, rHip_);
        if (ls && rs && lh && rh) {
            for (const auto& v : {*ls, *rs, *rh, *ls, *rh, *lh}) {
                put(torsos.vertices, v, alpha(col, 0.55));
            }
        }

        // Trails: the hips over the last seconds, and their trace on the
        // floor, fading; never across a gap.
        if (trails_) {
            const int now                 = static_cast<int>(frame - frames.constData());
            std::optional<QVector3D> next = hips(p);
            double nextAge                = 0.0;
            for (int k = now - 1; k >= 0; --k) {
                const double age = (frame->timestampNs - frames[k].timestampNs) / 1e9;
                if (age > kTrailSeconds) {
                    break;
                }
                std::optional<QVector3D> here;
                for (const auto& q : frames[k].people) {
                    if (q.trackId == p.trackId) {
                        here = hips(q);
                    }
                }
                if (here && next && age - nextAge <= kTrailGapS) {
                    const double a0 = 1.0 - nextAge / kTrailSeconds, a1 = 1.0 - age / kTrailSeconds;
                    put(trails.vertices, *next, alpha(col, 0.95 * a0));
                    put(trails.vertices, *here, alpha(col, 0.95 * a1));
                    put(trails.vertices, QVector3D(next->x(), 2.0f, next->z()),
                        alpha(col, 0.5 * a0));
                    put(trails.vertices, QVector3D(here->x(), 2.0f, here->z()),
                        alpha(col, 0.5 * a1));
                }
                if (here) {
                    next    = here;
                    nextAge = age;
                }
            }
        }
        if (p.trackId == follow_) {
            if (const auto h = hips(p)) {
                followTarget_ = QVector3D(h->x(), 1000.0f, h->z());
                followFound_  = true;
            }
        }
    }

    // The two people compared in Dyad Analysis: a line between their hips
    // and the distance, from raw positions as the Dyad numbers are.
    if (dyadA_ >= 0 && dyadB_ >= 0 && dyadA_ != dyadB_ && !hidden_.contains(dyadA_) &&
        !hidden_.contains(dyadB_)) {
        const Skeleton3DPerson *a = nullptr, *b = nullptr;
        for (const auto& q : frame->people) {
            a = q.trackId == dyadA_ ? &q : a;
            b = q.trackId == dyadB_ ? &q : b;
        }
        if (a && b) {
            const auto ha = hips(*a), hb = hips(*b);
            const auto raw = [&](const Skeleton3DPerson& p) -> std::optional<QVector3D> {
                if (lHip_ < 0 || rHip_ < 0 || lHip_ >= p.keypoints.size() ||
                    rHip_ >= p.keypoints.size() || !p.keypoints[lHip_].valid ||
                    !p.keypoints[rHip_].valid) {
                    return std::nullopt;
                }
                return (qv(p.keypoints[lHip_].positionRoom) + qv(p.keypoints[rHip_].positionRoom)) /
                       2.0f;
            };
            const auto ra = raw(*a), rb = raw(*b);
            if (ha && hb && ra && rb) {
                const QVector3D d = *hb - *ha;
                dyad_             = QVariantMap{
                                {"visible", true},
                                {"pos", (*ha + *hb) / 2.0f},
                                {"rot", QQuaternion::rotationTo(yAxis, d.normalized())},
                                {"len", d.length()},
                                {"label", QString("%1 m").arg((*ra - *rb).length() / 1000.0, 0, 'f', 2)}};
            }
        }
    }
    info_             = QString("tick %1  ·  %2 person(s) shown").arg(frame->tick).arg(shownCount);
    meshes_["trails"] = trails;
    meshes_["torsos"] = torsos;
    emit frameChanged();
}

} // namespace mosaic
