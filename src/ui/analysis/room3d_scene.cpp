#include "ui/analysis/room3d_scene.hpp"

#include <QQuaternion>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>

#include "ui/analysis/subject_colors.hpp"

namespace mosaic {

using room3d::alpha;
using room3d::put;
using room3d::qv;

namespace {

constexpr double kTrailSeconds = 2.0;
constexpr double kTrailGapS    = 0.25;  // a trail or a path never spans a longer gap
constexpr float kPathStepMm    = 20.0f; // a path's points at least this far apart

// Person N (1, 2, ...) in the Pose plugin's colour order (subject_colors.hpp).
QColor person_color(int person) { return subject_color(person - 1); }

QString pretty(const QString& name) {
    QString s = name;
    s.replace('_', ' ');
    return s;
}

} // namespace

Room3DScene::Room3DScene(QObject* parent) : Room3DMeshSource(parent) {}

QVariantList Room3DScene::people() const {
    QVariantList out;
    for (auto it = seconds_.cbegin(); it != seconds_.cend(); ++it) {
        out << QVariantMap{{"id", it.key()},
                           {"label", QString("Person %1").arg(it.key())},
                           {"seconds", it.value()},
                           {"color", color_of(it.key())},
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
    rebuild_paths();
    rebuild_frame();
}

void Room3DScene::set_position_ms(int64_t positionMs) {
    positionMs_ = positionMs;
    rebuild_frame();
}

void Room3DScene::share_stage(Room3DStage* stage) {
    stage_    = stage;
    ownsRoom_ = false;
}

void Room3DScene::set_time_ns(std::optional<int64_t> ns) {
    byTime_ = true;
    timeNs_ = ns;
    rebuild_frame();
}

void Room3DScene::set_matched(const QMap<int, QPair<QString, QColor>>& matched,
                              std::optional<QColor> others) {
    matched_ = matched;
    others_  = others;
    emit peopleChanged();
    rebuild_paths();
    rebuild_frame();
}

QColor Room3DScene::color_of(int person) const {
    const auto it = matched_.constFind(person);
    return it != matched_.cend() ? it->second : others_.value_or(person_color(person));
}

void Room3DScene::set_show_smoothed(bool smoothed) {
    smoothed_ = smoothed;
    rebuild_paths();
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
    rebuild_paths();
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

void Room3DScene::set_paths(bool on) {
    paths_ = on;
    rebuild_paths();
}

std::optional<QVector3D> Room3DScene::point(const Skeleton3DPerson& p, int i) const {
    if (i < 0 || i >= p.keypoints.size() || !p.keypoints[i].valid) {
        return std::nullopt;
    }
    const auto& kp = p.keypoints[i];
    return to_scene(smoothed_ ? kp.positionRoomSmoothed : kp.positionRoom);
}

std::optional<QVector3D> Room3DScene::hips(const Skeleton3DPerson& p) const {
    const auto a = point(p, lHip_), b = point(p, rHip_);
    return a && b ? std::optional<QVector3D>((*a + *b) / 2.0f) : std::nullopt;
}

// Each person's whole path: the hips, on the floor, over the recording;
// never across a gap. Rebuilt only when it can change (a new result, someone
// shown or hidden, smoothing), not each tick: it holds the whole recording.
void Room3DScene::rebuild_paths() {
    Mesh paths;
    if (paths_ && result_.is_valid()) {
        struct Last {
            QVector3D drawn;    // the path's last point
            int64_t seenNs = 0; // when the person was last seen
        };
        QMap<int, Last> last;
        for (const auto& f : result_.frames()) {
            for (const auto& p : f.people) {
                const auto h = hips(p);
                if (hidden_.contains(p.trackId) || !h) {
                    continue;
                }
                const QVector3D here(h->x(), 3.0f, h->z());
                auto it = last.find(p.trackId);
                if (it == last.end() || (f.timestampNs - it->seenNs) / 1e9 > kTrailGapS) {
                    last[p.trackId] = {here, f.timestampNs}; // a new piece
                    continue;
                }
                it->seenNs = f.timestampNs;
                // Points closer than a few centimetres add nothing visible
                // and would make a long recording's mesh large.
                if ((here - it->drawn).length() >= kPathStepMm) {
                    const QColor col = alpha(color_of(p.trackId), 0.85);
                    put(paths.vertices, it->drawn, col);
                    put(paths.vertices, here, col);
                    it->drawn = here;
                }
            }
        }
    }
    set_mesh("paths", std::move(paths));
    emit pathsChanged();
}

// What does not change during playback: the keypoints' places in the
// result, how long each person was in 3D, and the room.
void Room3DScene::rebuild_static() {
    seconds_.clear();
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
        if (ownsRoom_) {
            stage_->clear_room();
        }
        return;
    }

    // The room: up and the floor from the people, framed on them (not when
    // another page's room is shared).
    seconds_ = result_.seconds_by_person();
    if (!ownsRoom_) {
        return;
    }
    std::vector<Skeleton3DVec3> points;
    for (const auto& f : result_.frames()) {
        for (const auto& p : f.people) {
            for (const auto& kp : p.keypoints) {
                if (kp.valid) {
                    points.push_back(kp.positionRoom);
                }
            }
        }
    }
    stage_->set_room(Room3DStage::basis_of(result_), result_.cameras(), points);
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
    tick_        = -1;

    const auto& frames           = result_.frames();
    const Skeleton3DFrame* frame = nullptr;
    if (result_.is_valid() && !frames.isEmpty()) {
        if (!byTime_) {
            frame = result_.nearest_frame(frames.first().timestampNs + positionMs_ * 1000000LL);
        } else if (timeNs_) {
            frame                         = result_.nearest_frame(*timeNs_);
            constexpr int64_t kSameTimeNs = 100000000; // further: nobody in 3D then
            if (frame && std::llabs(frame->timestampNs - *timeNs_) > kSameTimeNs) {
                frame = nullptr;
            }
        }
    }
    if (!frame) {
        set_mesh("trails", std::move(trails));
        set_mesh("torsos", std::move(torsos));
        emit frameChanged();
        return;
    }
    tick_             = frame->tick;
    const auto& names = result_.keypoint_names();
    const auto pos    = [this](const Skeleton3DPerson& p, int i) { return point(p, i); };
    const QVector3D yAxis(0, 1, 0);

    int shownCount = 0;
    for (const auto& p : frame->people) {
        if (hidden_.contains(p.trackId)) {
            continue;
        }
        ++shownCount;
        const QColor col     = color_of(p.trackId);
        const bool elsewhere = headsElsewhere_.contains(p.trackId);
        const QString who    = matched_.contains(p.trackId) ? matched_.value(p.trackId).first
                                                            : QString("Person %1").arg(p.trackId);

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
        if (headN && !elsewhere) { // else the other page's head
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
        if (topY > -1e29f && !elsewhere) {
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
    info_ = QString("tick %1  ·  %2 person(s) shown").arg(frame->tick).arg(shownCount);
    set_mesh("trails", std::move(trails));
    set_mesh("torsos", std::move(torsos));
    emit frameChanged();
}

} // namespace mosaic
