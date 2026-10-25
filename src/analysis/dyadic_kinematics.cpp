#include "analysis/dyadic_kinematics.hpp"

#include <QList>
#include <QMap>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <vector>

#include "analysis/room_view_math.hpp"

namespace mosaic {

namespace {

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

Vec3 to_vec3(const Skeleton3DVec3& a) { return {a[0], a[1], a[2]}; }
Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }
double distance(const Vec3& a, const Vec3& b) { return norm(a - b); }

const Skeleton3DPerson* find_person(const Skeleton3DFrame& frame, int trackId) {
    for (const auto& p : frame.people) {
        if (p.trackId == trackId) {
            return &p;
        }
    }
    return nullptr;
}

// Hip midpoint, only if both hip keypoints are present and valid.
std::optional<Vec3> hip_midpoint(const Skeleton3DPerson& person, int idxLHip, int idxRHip) {
    if (idxLHip < 0 || idxRHip < 0 || idxLHip >= person.keypoints.size() ||
        idxRHip >= person.keypoints.size()) {
        return std::nullopt;
    }
    const auto& lh = person.keypoints[idxLHip];
    const auto& rh = person.keypoints[idxRHip];
    if (!lh.valid || !rh.valid) {
        return std::nullopt;
    }
    return (to_vec3(lh.positionRoom) + to_vec3(rh.positionRoom)) * 0.5;
}

// "Chest-forward" unit vector, resolving the shoulder x spine cross
// product's inherent front/back ambiguity via the nose — see
// dyadic_kinematics.hpp's own doc comment for the full derivation and its
// documented interpretation caveat. Requires nose + both shoulders + both
// hips valid; returns nullopt otherwise (including the degenerate case
// where the spine/shoulder vectors are near-parallel, e.g. someone bent
// fully forward).
std::optional<Vec3> facing_vector(const Skeleton3DPerson& person, int idxNose, int idxLShoulder,
                                  int idxRShoulder, int idxLHip, int idxRHip) {
    if (idxNose < 0 || idxLShoulder < 0 || idxRShoulder < 0 || idxNose >= person.keypoints.size() ||
        idxLShoulder >= person.keypoints.size() || idxRShoulder >= person.keypoints.size()) {
        return std::nullopt;
    }
    const auto& nose = person.keypoints[idxNose];
    const auto& ls   = person.keypoints[idxLShoulder];
    const auto& rs   = person.keypoints[idxRShoulder];
    if (!nose.valid || !ls.valid || !rs.valid) {
        return std::nullopt;
    }
    const auto hipMid = hip_midpoint(person, idxLHip, idxRHip);
    if (!hipMid) {
        return std::nullopt;
    }

    const Vec3 shoulderMid = (to_vec3(ls.positionRoom) + to_vec3(rs.positionRoom)) * 0.5;
    const Vec3 shoulderVec = to_vec3(rs.positionRoom) - to_vec3(ls.positionRoom);
    const Vec3 spineVec    = shoulderMid - *hipMid;
    Vec3 rawNormal         = cross(spineVec, shoulderVec);
    const double n         = norm(rawNormal);
    if (n < 1e-6) {
        return std::nullopt;
    }

    // The initial cross-product argument order (and therefore rawNormal's
    // raw sign) is irrelevant — whichever direction it happens to point,
    // this check flips it to point toward the person's own nose, which is
    // always on the chest side of the torso plane, never the back.
    const Vec3 noseOffset = to_vec3(nose.positionRoom) - shoulderMid;
    if (dot(rawNormal, noseOffset) < 0.0) {
        rawNormal = rawNormal * -1.0;
    }
    return rawNormal * (1.0 / n);
}

// Derivative of a person's own valid hip-midpoint sequence (sparse, keyed
// by frame index — may have gaps relative to the full frame timeline),
// using the real elapsed time between the two nearest valid samples.
// Mirrors pose_kinematics.cpp's exact "skip missing, real dt" discipline,
// applied to a 3D centroid instead of a single 2D keypoint.
QMap<int, double> derive_speed(const QMap<int, Vec3>& hipMidByIdx,
                               const QVector<DyadicSample>& samples) {
    QMap<int, double> speed;
    const QList<int> idxs = hipMidByIdx.keys();
    for (int k = 1; k < idxs.size(); ++k) {
        const int iPrev = idxs[k - 1];
        const int iCur  = idxs[k];
        const double dt = (samples[iCur].timestampNs - samples[iPrev].timestampNs) / 1e9;
        if (dt > 0.0) {
            speed[iCur] = distance(hipMidByIdx[iCur], hipMidByIdx[iPrev]) / dt;
        }
    }
    return speed;
}

// The middle value (the upper one of an even count), NaN when there is none.
double median(std::vector<double> v) {
    if (v.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

// The value at `share` (0 to 1) of the sorted values, by nearest rank.
double rank_value(std::vector<double> v, double share) {
    if (v.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double rank = std::ceil(share * static_cast<double>(v.size())); // 1-based
    const auto i = static_cast<size_t>(std::clamp(rank, 1.0, static_cast<double>(v.size()))) - 1;
    std::nth_element(v.begin(), v.begin() + i, v.end());
    return v[i];
}

// The angle between two directions in degrees, NaN when either has no
// length (two points on top of each other).
double angle_deg(const room_view::Vec3& a, const room_view::Vec3& b) {
    const double na = room_view::norm(a);
    const double nb = room_view::norm(b);
    if (na < 1e-9 || nb < 1e-9) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return std::acos(std::clamp(room_view::dot(a, b) / (na * nb), -1.0, 1.0)) * 180.0 /
           std::numbers::pi;
}

} // namespace

QVector<Posture> classify_posture(const QVector<PersonSample>& samples) {
    constexpr double kSittingBelowMm = 800.0;
    constexpr double kWalkingMmPerS  = 350.0;
    constexpr int64_t kSpeedWindowNs = 500000000; // 0.5 s either side
    constexpr int64_t kModeWindowNs  = 500000000;
    const int n                      = static_cast<int>(samples.size());
    QVector<Posture> out(n, Posture::Unknown);

    // Each tick on its own, the speed as its neighbourhood's median.
    int lo = 0, hi = 0;
    for (int k = 0; k < n; ++k) {
        const PersonSample& s = samples[k];
        if (s.cameras == 0 || std::isnan(s.hipHeightMm)) {
            continue;
        }
        if (s.hipHeightMm < kSittingBelowMm) {
            out[k] = Posture::Sitting;
            continue;
        }
        while (samples[lo].timestampNs < s.timestampNs - kSpeedWindowNs) {
            ++lo;
        }
        while (hi + 1 < n && samples[hi + 1].timestampNs <= s.timestampNs + kSpeedWindowNs) {
            ++hi;
        }
        std::vector<double> speeds;
        for (int j = lo; j <= hi; ++j) {
            if (!std::isnan(samples[j].speedMmPerS)) {
                speeds.push_back(samples[j].speedMmPerS);
            }
        }
        const double v = median(speeds);
        if (!std::isnan(v)) {
            out[k] = v >= kWalkingMmPerS ? Posture::Walking : Posture::Standing;
        }
    }

    // Then each tick takes the most common posture among the ticks within
    // 0.5 s either side that have one (a tie keeps its own): a mode filter,
    // so a posture held under about half a second does not show. Ticks
    // without one (no hips) stay Unknown: never filled in.
    QVector<Posture> smoothed = out;
    lo = 0, hi = 0;
    for (int k = 0; k < n; ++k) {
        if (out[k] == Posture::Unknown) {
            continue;
        }
        const int64_t t = samples[k].timestampNs;
        while (samples[lo].timestampNs < t - kModeWindowNs) {
            ++lo;
        }
        while (hi + 1 < n && samples[hi + 1].timestampNs <= t + kModeWindowNs) {
            ++hi;
        }
        std::array<int, 4> votes{};
        for (int j = std::min(lo, k); j <= std::max(hi, k); ++j) {
            ++votes[static_cast<int>(out[j])];
        }
        votes[static_cast<int>(Posture::Unknown)] = 0;
        int best                                  = static_cast<int>(out[k]);
        for (int v = 1; v < 4; ++v) {
            if (votes[v] > votes[best]) {
                best = v;
            }
        }
        smoothed[k] = static_cast<Posture>(best);
    }
    return smoothed;
}

Spread spread_of(const QVector<PersonSample>& samples,
                 const std::function<double(const PersonSample&)>& value) {
    std::vector<double> v;
    for (const auto& s : samples) {
        if (const double x = value(s); !std::isnan(x)) {
            v.push_back(x);
        }
    }
    return {median(v), rank_value(v, 0.95)};
}

PersonKinematicsSeries compute_person_kinematics(const Skeleton3DResult& result, int personId) {
    PersonKinematicsSeries out;
    const auto& frames = result.frames();
    if (!result.is_valid() || frames.isEmpty()) {
        return out;
    }
    const auto& names = result.keypoint_names();
    const int lh      = static_cast<int>(names.indexOf("left_hip"));
    const int rh      = static_cast<int>(names.indexOf("right_hip"));
    QVector<int> head;
    for (const char* n : {"nose", "left_eye", "right_eye", "left_ear", "right_ear"}) {
        if (const int i = static_cast<int>(names.indexOf(n)); i >= 0) {
            head << i;
        }
    }
    const auto at_name = [&](const char* name) { return static_cast<int>(names.indexOf(name)); };
    const int ls = at_name("left_shoulder"), rs = at_name("right_shoulder");
    const int le = at_name("left_elbow"), re = at_name("right_elbow");
    const int lw = at_name("left_wrist"), rw = at_name("right_wrist");
    const int lk = at_name("left_knee"), rk = at_name("right_knee");
    const int la = at_name("left_ankle"), ra = at_name("right_ankle");
    const int lear = at_name("left_ear"), rear = at_name("right_ear");
    const room_view::Vec3 up  = room_view::estimate_up(result);
    const double floor        = room_view::floor_height(result, up);
    constexpr int64_t kNearNs = 250000000; // 0.25 s: neighbours closer than this

    // The hip midpoint per tick, when the person is there with both hips,
    // and every keypoint's smoothed position (empty where the person is not
    // in 3D), for the rates below.
    const int n = static_cast<int>(frames.size());
    std::vector<std::optional<room_view::Vec3>> hips(n);
    std::vector<std::vector<std::optional<room_view::Vec3>>> joints(n);
    std::vector<double> heads;
    out.samples.resize(n);
    int present = 0;
    for (int k = 0; k < n; ++k) {
        PersonSample& s = out.samples[k];
        s.timestampNs   = frames[k].timestampNs;
        const auto* p   = find_person(frames[k], personId);
        if (!p) {
            continue;
        }
        ++present;
        s.cameras      = p->numContributingCameras;
        const auto& kp = p->keypoints;
        if (lh >= 0 && rh >= 0 && lh < kp.size() && rh < kp.size() && kp[lh].valid &&
            kp[rh].valid) {
            hips[k] =
                room_view::scale(room_view::add(kp[lh].positionRoom, kp[rh].positionRoom), 0.5);
            s.hipHeightMm = room_view::dot(*hips[k], up) - floor;
        }
        for (const int i : head) {
            if (i < kp.size() && kp[i].valid) {
                const double h = room_view::dot(kp[i].positionRoom, up) - floor;
                s.headHeightMm = std::isnan(s.headHeightMm) ? h : std::max(s.headHeightMm, h);
            }
        }
        if (!std::isnan(s.headHeightMm)) {
            heads.push_back(s.headHeightMm);
        }

        // Joint angles, from the smoothed keypoints: a 3D keypoint jitters
        // by a few centimetres, a few degrees on a forearm.
        const auto pos = [&](int i) -> std::optional<room_view::Vec3> {
            if (i < 0 || i >= kp.size() || !kp[i].valid) {
                return std::nullopt;
            }
            return kp[i].positionRoomSmoothed;
        };
        const auto mid = [&](int a, int b) -> std::optional<room_view::Vec3> {
            const auto pa = pos(a);
            const auto pb = pos(b);
            if (!pa || !pb) {
                return std::nullopt;
            }
            return room_view::scale(room_view::add(*pa, *pb), 0.5);
        };
        // Down the trunk: from the shoulders' midpoint to the hips'.
        std::optional<room_view::Vec3> down;
        if (const auto sh = mid(ls, rs), hp = mid(lh, rh); sh && hp) {
            down = room_view::sub(*hp, *sh);
        }
        // At b, between a to b and b to c: 0 when a, b and c are in line.
        const auto bend = [&](int a, int b, int c) {
            const auto pa = pos(a);
            const auto pb = pos(b);
            const auto pc = pos(c);
            return pa && pb && pc ? angle_deg(room_view::sub(*pb, *pa), room_view::sub(*pc, *pb))
                                  : std::numeric_limits<double>::quiet_NaN();
        };
        // Between the trunk, downwards, and a to b: 0 when a to b hangs along it.
        const auto from_trunk = [&](int a, int b) {
            const auto pa = pos(a);
            const auto pb = pos(b);
            return down && pa && pb ? angle_deg(*down, room_view::sub(*pb, *pa))
                                    : std::numeric_limits<double>::quiet_NaN();
        };
        s.kneeFlexionDeg  = {bend(lh, lk, la), bend(rh, rk, ra)};
        s.elbowFlexionDeg = {bend(ls, le, lw), bend(rs, re, rw)};
        s.hipFlexionDeg   = {from_trunk(lh, lk), from_trunk(rh, rk)};
        s.armRaiseDeg     = {from_trunk(ls, le), from_trunk(rs, re)};
        joints[k].resize(kp.size());
        for (int i = 0; i < kp.size(); ++i) {
            joints[k][i] = pos(i);
        }
    }

    // Rates by central difference: at tick k, from tick k-1 to tick k+1,
    // when both are within 0.25 s of k and the person is in 3D at all three
    // (never across a gap).
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    const auto span_s     = [&](int k) {
        if (k < 1 || k + 1 >= n || joints[k - 1].empty() || joints[k].empty() ||
            joints[k + 1].empty()) {
            return kNaN;
        }
        const int64_t d = frames[k + 1].timestampNs - frames[k - 1].timestampNs;
        return d > 0 && d <= 2 * kNearNs ? static_cast<double>(d) / 1e9 : kNaN;
    };
    const auto joint = [&](int k, int i) -> std::optional<room_view::Vec3> {
        return i >= 0 && i < static_cast<int>(joints[k].size()) ? joints[k][i] : std::nullopt;
    };
    const auto ears = [&](int k) -> std::optional<std::pair<room_view::Vec3, room_view::Vec3>> {
        const auto l = joint(k, lear);
        const auto r = joint(k, rear);
        if (!l || !r) {
            return std::nullopt;
        }
        return std::pair{*l, *r};
    };
    // How fast a point moved, when it is known at k and at both neighbours.
    const auto moved = [](const auto& before, const auto& now, const auto& after, double dt) {
        return before && now && after && !std::isnan(dt)
                   ? room_view::norm(room_view::sub(*after, *before)) / dt
                   : kNaN;
    };
    const auto changed = [](double before, double now, double after, double dt) {
        return std::isnan(now) ? kNaN : (after - before) / dt; // NaN in, NaN out
    };
    for (int k = 0; k < n; ++k) {
        PersonSample& s = out.samples[k];
        const double dt = span_s(k);
        if (!std::isnan(dt)) {
            s.speedMmPerS       = moved(hips[k - 1], hips[k], hips[k + 1], dt);
            const auto speed_of = [&](int i) {
                return moved(joint(k - 1, i), joint(k, i), joint(k + 1, i), dt);
            };
            s.wristSpeedMmPerS       = {speed_of(lw), speed_of(rw)};
            const auto& b            = out.samples[k - 1];
            const auto& a            = out.samples[k + 1];
            s.kneeFlexionRateDegPerS = {
                changed(b.kneeFlexionDeg.left, s.kneeFlexionDeg.left, a.kneeFlexionDeg.left, dt),
                changed(b.kneeFlexionDeg.right, s.kneeFlexionDeg.right, a.kneeFlexionDeg.right,
                        dt)};
            s.elbowFlexionRateDegPerS = {
                changed(b.elbowFlexionDeg.left, s.elbowFlexionDeg.left, a.elbowFlexionDeg.left, dt),
                changed(b.elbowFlexionDeg.right, s.elbowFlexionDeg.right, a.elbowFlexionDeg.right,
                        dt)};
            double sum = 0.0;
            int seen   = 0;
            for (int i = 0; i < static_cast<int>(joints[k].size()); ++i) {
                if (const double v = speed_of(i); !std::isnan(v)) {
                    sum += v;
                    ++seen;
                }
            }
            s.motionMmPerS = seen > 0 ? sum / seen : kNaN;
            if (const auto e0 = ears(k - 1), e = ears(k), e1 = ears(k + 1); e0 && e && e1) {
                const auto mid = [](const auto& p) {
                    return room_view::scale(room_view::add(p.first, p.second), 0.5);
                };
                s.headSpeedMmPerS = room_view::norm(room_view::sub(mid(*e1), mid(*e0))) / dt;
                s.headTurnDegPerS = angle_deg(room_view::sub(e0->second, e0->first),
                                              room_view::sub(e1->second, e1->first)) /
                                    dt;
            }
        }
        const double v = s.speedMmPerS;
        if (!std::isnan(v) &&
            (std::isnan(out.stats.topSpeedMmPerS) || v > out.stats.topSpeedMmPerS)) {
            out.stats.topSpeedMmPerS = v;
        }
    }
    const QVector<Posture> postures = classify_posture(out.samples);
    for (int k = 0; k < n; ++k) {
        out.samples[k].posture = postures[k];
    }
    if (n > 1) {
        const double stepS =
            static_cast<double>(frames.last().timestampNs - frames.first().timestampNs) / 1e9 /
            (n - 1);
        out.stats.secondsSeen = present * stepS;
        for (const auto& s : out.samples) {
            if (s.cameras > 0) {
                out.stats.secondsByPosture[static_cast<int>(s.posture)] += stepS;
            }
        }
    }
    out.stats.medianHeadHeightMm = median(heads);
    const auto median_of         = [&](const LeftRight PersonSample::*joint) {
        std::vector<double> left, right;
        for (const auto& s : out.samples) {
            if (!std::isnan((s.*joint).left)) {
                left.push_back((s.*joint).left);
            }
            if (!std::isnan((s.*joint).right)) {
                right.push_back((s.*joint).right);
            }
        }
        return LeftRight{median(left), median(right)};
    };
    out.stats.medianKneeFlexionDeg  = median_of(&PersonSample::kneeFlexionDeg);
    out.stats.medianHipFlexionDeg   = median_of(&PersonSample::hipFlexionDeg);
    out.stats.medianElbowFlexionDeg = median_of(&PersonSample::elbowFlexionDeg);
    out.stats.medianArmRaiseDeg     = median_of(&PersonSample::armRaiseDeg);
    return out;
}

DyadicKinematicsSeries compute_dyadic_kinematics(const Skeleton3DResult& result, int trackIdA,
                                                 int trackIdB, int congruentMotionWindow) {
    DyadicKinematicsSeries out;
    if (!result.is_valid() || result.frames().isEmpty() || trackIdA == trackIdB) {
        return out;
    }

    const auto& names      = result.keypoint_names();
    const int idxNose      = static_cast<int>(names.indexOf("nose"));
    const int idxLShoulder = static_cast<int>(names.indexOf("left_shoulder"));
    const int idxRShoulder = static_cast<int>(names.indexOf("right_shoulder"));
    const int idxLHip      = static_cast<int>(names.indexOf("left_hip"));
    const int idxRHip      = static_cast<int>(names.indexOf("right_hip"));

    const int n = result.frames().size();
    out.samples.resize(n);
    for (int i = 0; i < n; ++i) {
        out.samples[i].timestampNs = result.frames()[i].timestampNs;
    }

    // Phase 1: per-frame distance + facing, and each person's own hip-mid
    // sequence (collected for phase 3's speed derivation) — sparse
    // frame-index -> value maps, since A, B, and "both facing-ready" may
    // each be valid at a different subset of frames.
    QMap<int, double> distByIdx;
    QMap<int, double> faceByIdx;
    QMap<int, Vec3> hipMidAByIdx;
    QMap<int, Vec3> hipMidBByIdx;

    for (int i = 0; i < n; ++i) {
        const auto& frame   = result.frames()[i];
        const auto* personA = find_person(frame, trackIdA);
        const auto* personB = find_person(frame, trackIdB);

        // Each person's own hip midpoint is recorded whenever THAT person
        // alone is present and valid — never gated on the other person's
        // presence. This matters for phase 3's speed derivation: if it
        // were gated on both (as an earlier version of this function
        // mistakenly was), one person briefly leaving frame would corrupt
        // the OTHER, still-continuously-tracked person's own speed series
        // too, silently widening their real per-frame motion into a
        // multi-frame average and biasing congruent motion. Distance/facing
        // remain correctly gated on both, since those are inherently
        // pairwise — see the two `if (hipA && hipB)`/`if (personA &&
        // personB)` checks below.
        std::optional<Vec3> hipA;
        std::optional<Vec3> hipB;
        if (personA) {
            hipA = hip_midpoint(*personA, idxLHip, idxRHip);
            if (hipA) {
                hipMidAByIdx[i] = *hipA;
            }
        }
        if (personB) {
            hipB = hip_midpoint(*personB, idxLHip, idxRHip);
            if (hipB) {
                hipMidBByIdx[i] = *hipB;
            }
        }
        if (hipA && hipB) {
            distByIdx[i] = distance(*hipA, *hipB);
        }

        if (personA && personB) {
            const auto fwdA =
                facing_vector(*personA, idxNose, idxLShoulder, idxRShoulder, idxLHip, idxRHip);
            const auto fwdB =
                facing_vector(*personB, idxNose, idxLShoulder, idxRShoulder, idxLHip, idxRHip);
            if (fwdA && fwdB) {
                faceByIdx[i] = dot(*fwdA, *fwdB);
            }
        }
    }

    for (auto it = distByIdx.constBegin(); it != distByIdx.constEnd(); ++it) {
        out.samples[it.key()].distanceMm = it.value();
    }
    for (auto it = faceByIdx.constBegin(); it != faceByIdx.constEnd(); ++it) {
        out.samples[it.key()].facingCosine = it.value();
    }

    // Phase 2: approach rate — derivative of distByIdx across its own valid
    // index sequence, real-dt discipline, mirroring pose_kinematics.cpp.
    {
        const QList<int> idxs = distByIdx.keys();
        for (int k = 1; k < idxs.size(); ++k) {
            const int iPrev = idxs[k - 1];
            const int iCur  = idxs[k];
            const double dt =
                (out.samples[iCur].timestampNs - out.samples[iPrev].timestampNs) / 1e9;
            if (dt > 0.0) {
                out.samples[iCur].approachRateMmPerS = (distByIdx[iCur] - distByIdx[iPrev]) / dt;
            }
        }
    }

    // Phase 3: each person's own instantaneous speed, independently.
    const QMap<int, double> speedA = derive_speed(hipMidAByIdx, out.samples);
    const QMap<int, double> speedB = derive_speed(hipMidBByIdx, out.samples);

    // Phase 4: congruent motion — trailing Pearson correlation of A's and
    // B's speed, evaluated at every frame index where both have a defined
    // instantaneous speed ("paired").
    QList<int> pairedIdxs;
    for (auto it = speedA.constBegin(); it != speedA.constEnd(); ++it) {
        if (speedB.contains(it.key())) {
            pairedIdxs.append(it.key());
        }
    }
    std::sort(pairedIdxs.begin(), pairedIdxs.end());

    const int window = std::max(1, congruentMotionWindow);
    for (int k = 0; k < pairedIdxs.size(); ++k) {
        const int lo    = std::max(0, k - window + 1);
        const int count = k - lo + 1;
        if (count < 3) {
            continue;
        }

        double meanA = 0.0;
        double meanB = 0.0;
        for (int j = lo; j <= k; ++j) {
            meanA += speedA[pairedIdxs[j]];
            meanB += speedB[pairedIdxs[j]];
        }
        meanA /= count;
        meanB /= count;

        double cov  = 0.0;
        double varA = 0.0;
        double varB = 0.0;
        for (int j = lo; j <= k; ++j) {
            const double da = speedA[pairedIdxs[j]] - meanA;
            const double db = speedB[pairedIdxs[j]] - meanB;
            cov += da * db;
            varA += da * da;
            varB += db * db;
        }
        if (varA > 1e-12 && varB > 1e-12) {
            out.samples[pairedIdxs[k]].congruentMotionCorr = cov / std::sqrt(varA * varB);
        }
    }

    // Stats.
    double sumDist = 0.0;
    double minDist = std::numeric_limits<double>::max();
    double maxDist = std::numeric_limits<double>::lowest();
    int distCount  = 0;
    double sumFace = 0.0;
    int faceCount  = 0;
    double sumCorr = 0.0;
    int corrCount  = 0;
    for (const auto& s : out.samples) {
        if (!std::isnan(s.distanceMm)) {
            sumDist += s.distanceMm;
            minDist = std::min(minDist, s.distanceMm);
            maxDist = std::max(maxDist, s.distanceMm);
            ++distCount;
        }
        if (!std::isnan(s.facingCosine)) {
            sumFace += s.facingCosine;
            ++faceCount;
        }
        if (!std::isnan(s.congruentMotionCorr)) {
            sumCorr += s.congruentMotionCorr;
            ++corrCount;
        }
    }
    if (distCount > 0) {
        out.stats.meanDistanceMm = sumDist / distCount;
        out.stats.minDistanceMm  = minDist;
        out.stats.maxDistanceMm  = maxDist;
    }
    if (faceCount > 0) {
        out.stats.meanFacingCosine = sumFace / faceCount;
    }
    if (corrCount > 0) {
        out.stats.meanCongruentMotionCorr = sumCorr / corrCount;
    }
    out.stats.pctTicksBothPresent = n > 0 ? static_cast<double>(distCount) / n : 0.0;

    return out;
}

} // namespace mosaic
