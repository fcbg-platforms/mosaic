#include "analysis/dyadic_kinematics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace mosaic {

namespace {

using V3 = std::array<double, 3>;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

V3 v3_sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }

V3 v3_add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }

V3 v3_scale(const V3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }

double v3_dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

V3 v3_cross(const V3& a, const V3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

double v3_norm(const V3& a) { return std::sqrt(v3_dot(a, a)); }

const Skeleton3DPerson* find_person(const Skeleton3DFrame& frame, int trackId) {
    for (const auto& p : frame.people) {
        if (p.trackId == trackId) {
            return &p;
        }
    }
    return nullptr;
}

// Average of left_hip/right_hip's raw positionRoom — nullopt unless both
// indices resolved and both keypoints are valid at this person/frame.
std::optional<V3> hip_midpoint(const Skeleton3DPerson& person, int idxLHip, int idxRHip) {
    if (idxLHip < 0 || idxRHip < 0) {
        return std::nullopt;
    }
    if (idxLHip >= person.keypoints.size() || idxRHip >= person.keypoints.size()) {
        return std::nullopt;
    }
    const auto& l = person.keypoints[idxLHip];
    const auto& r = person.keypoints[idxRHip];
    if (!l.valid || !r.valid) {
        return std::nullopt;
    }
    return v3_scale(v3_add(l.positionRoom, r.positionRoom), 0.5);
}

// Torso-forward unit vector, resolved with a nose-relative sign correction
// so the raw cross(spine, shoulders) product's own handedness never matters
// — see dyadic_kinematics.hpp's doc comment. nullopt if nose/shoulders/hips
// aren't all valid, or if the spine/shoulder vectors are too close to
// parallel to define a torso plane at all (e.g. someone bent fully forward).
std::optional<V3> facing_forward(const Skeleton3DPerson& person, int idxNose, int idxLShoulder,
                                 int idxRShoulder, const V3& hipMid) {
    if (idxNose < 0 || idxLShoulder < 0 || idxRShoulder < 0) {
        return std::nullopt;
    }
    if (idxNose >= person.keypoints.size() || idxLShoulder >= person.keypoints.size() ||
        idxRShoulder >= person.keypoints.size()) {
        return std::nullopt;
    }
    const auto& nose = person.keypoints[idxNose];
    const auto& lSh  = person.keypoints[idxLShoulder];
    const auto& rSh  = person.keypoints[idxRShoulder];
    if (!nose.valid || !lSh.valid || !rSh.valid) {
        return std::nullopt;
    }

    const V3 shoulderMid = v3_scale(v3_add(lSh.positionRoom, rSh.positionRoom), 0.5);
    const V3 shoulderVec = v3_sub(rSh.positionRoom, lSh.positionRoom);
    const V3 spineVec    = v3_sub(shoulderMid, hipMid);

    V3 rawNormal      = v3_cross(spineVec, shoulderVec);
    const double norm = v3_norm(rawNormal);
    if (norm < 1e-6) {
        return std::nullopt; // degenerate: spine/shoulders too near-parallel
    }

    // Self-correcting sign resolution: whichever side of the torso plane the
    // nose sits on IS the chest-forward direction, regardless of which way
    // the raw cross product happened to point.
    const V3 towardNose = v3_sub(nose.positionRoom, shoulderMid);
    if (v3_dot(rawNormal, towardNose) < 0.0) {
        rawNormal = v3_scale(rawNormal, -1.0);
    }
    return v3_scale(rawNormal, 1.0 / norm);
}

// Real-Δt-between-nearest-valid-samples speed derivative, applied to one
// person's own per-tick hip-midpoint sequence (independent of the other
// person's presence — the same "skip missing, use real elapsed time"
// discipline pose_kinematics.hpp's compute_kinematics() already established,
// applied here to a scalar-distance / 3D-position signal instead of a 2D
// keypoint).
QVector<double> compute_speed(const QVector<std::optional<V3>>& hipMid,
                              const QVector<int64_t>& timestampNs) {
    const int n = hipMid.size();
    QVector<double> speed(n, kNaN);
    int prevIdx = -1;
    for (int i = 0; i < n; ++i) {
        if (!hipMid[i]) {
            continue;
        }
        if (prevIdx >= 0) {
            const double dt = static_cast<double>(timestampNs[i] - timestampNs[prevIdx]) / 1e9;
            if (dt > 0.0) {
                speed[i] = v3_norm(v3_sub(*hipMid[i], *hipMid[prevIdx])) / dt;
            }
        }
        prevIdx = i;
    }
    return speed;
}

// Pearson correlation coefficient of two equal-length series. NaN if either
// series has zero variance (e.g. all-constant) — undefined, not zero.
double pearson_r(const QVector<double>& a, const QVector<double>& b) {
    const int n  = a.size();
    double meanA = 0.0, meanB = 0.0;
    for (int i = 0; i < n; ++i) {
        meanA += a[i];
        meanB += b[i];
    }
    meanA /= n;
    meanB /= n;

    double num = 0.0, denA = 0.0, denB = 0.0;
    for (int i = 0; i < n; ++i) {
        const double da = a[i] - meanA;
        const double db = b[i] - meanB;
        num += da * db;
        denA += da * da;
        denB += db * db;
    }
    if (denA <= 0.0 || denB <= 0.0) {
        return kNaN;
    }
    return num / std::sqrt(denA * denB);
}

} // namespace

DyadicKinematicsSeries compute_dyadic_kinematics(const Skeleton3DResult& result, int trackIdA,
                                                 int trackIdB, int congruentMotionWindow) {
    DyadicKinematicsSeries out;
    if (!result.is_valid()) {
        return out;
    }

    const auto& names    = result.keypoint_names();
    const int idxNose    = names.indexOf("nose");
    const int idxLSh     = names.indexOf("left_shoulder");
    const int idxRSh     = names.indexOf("right_shoulder");
    const int idxLHip    = names.indexOf("left_hip");
    const int idxRHip    = names.indexOf("right_hip");
    const bool hasHips   = idxLHip >= 0 && idxRHip >= 0;
    const bool hasFacing = hasHips && idxNose >= 0 && idxLSh >= 0 && idxRSh >= 0;

    const int n = result.frames().size();
    out.samples.resize(n);

    QVector<std::optional<V3>> hipA(n), hipB(n);
    // Person pointers resolved once here and reused below (distance/facing
    // loop) rather than re-scanning frame.people a second time per tick —
    // frame.people/find_person() aren't retained past this function call, so
    // caching the pointer (not just its derived hip midpoint) is safe: both
    // loops run against the same still-alive `result` reference.
    QVector<const Skeleton3DPerson*> personsA(n, nullptr), personsB(n, nullptr);
    QVector<int64_t> timestampNs(n);
    for (int i = 0; i < n; ++i) {
        const auto& frame          = result.frames()[i];
        timestampNs[i]             = frame.timestampNs;
        out.samples[i].timestampNs = frame.timestampNs;
        if (!hasHips) {
            continue;
        }
        personsA[i] = find_person(frame, trackIdA);
        personsB[i] = find_person(frame, trackIdB);
        if (personsA[i]) {
            hipA[i] = hip_midpoint(*personsA[i], idxLHip, idxRHip);
        }
        if (personsB[i]) {
            hipB[i] = hip_midpoint(*personsB[i], idxLHip, idxRHip);
        }
    }

    // Distance + facing, only for ticks where both people's hips are valid.
    int bothPresentCount = 0;
    for (int i = 0; i < n; ++i) {
        if (!hipA[i] || !hipB[i]) {
            continue;
        }
        ++bothPresentCount;
        out.samples[i].distanceMm = v3_norm(v3_sub(*hipA[i], *hipB[i]));

        if (hasFacing) {
            const auto fwdA = personsA[i]
                                  ? facing_forward(*personsA[i], idxNose, idxLSh, idxRSh, *hipA[i])
                                  : std::nullopt;
            const auto fwdB = personsB[i]
                                  ? facing_forward(*personsB[i], idxNose, idxLSh, idxRSh, *hipB[i])
                                  : std::nullopt;
            if (fwdA && fwdB) {
                out.samples[i].facingCosine = v3_dot(*fwdA, *fwdB);
            }
        }
    }

    // Approach rate: real-Δt derivative between the two nearest ticks with a
    // valid distanceMm (never a fabricated value across a gap between them).
    {
        int prevIdx = -1;
        for (int i = 0; i < n; ++i) {
            if (std::isnan(out.samples[i].distanceMm)) {
                continue;
            }
            if (prevIdx >= 0) {
                const double dt = static_cast<double>(timestampNs[i] - timestampNs[prevIdx]) / 1e9;
                if (dt > 0.0) {
                    out.samples[i].approachRateMmPerS =
                        (out.samples[i].distanceMm - out.samples[prevIdx].distanceMm) / dt;
                }
            }
            prevIdx = i;
        }
    }

    // Congruent motion: each person's own instantaneous speed, independent
    // of the other's presence, then a rolling Pearson r over the trailing
    // window of ticks where BOTH speeds are defined.
    const QVector<double> speedA = compute_speed(hipA, timestampNs);
    const QVector<double> speedB = compute_speed(hipB, timestampNs);
    {
        QVector<double> windowA, windowB;
        windowA.reserve(congruentMotionWindow > 0 ? congruentMotionWindow : 0);
        windowB.reserve(congruentMotionWindow > 0 ? congruentMotionWindow : 0);
        for (int i = 0; i < n; ++i) {
            if (std::isnan(speedA[i]) || std::isnan(speedB[i])) {
                continue;
            }
            windowA.append(speedA[i]);
            windowB.append(speedB[i]);
            if (congruentMotionWindow > 0 && windowA.size() > congruentMotionWindow) {
                windowA.removeFirst();
                windowB.removeFirst();
            }
            if (windowA.size() >= 3) {
                out.samples[i].congruentMotionCorr = pearson_r(windowA, windowB);
            }
        }
    }

    // Stats.
    double sumD = 0.0, minD = std::numeric_limits<double>::max();
    double maxD = std::numeric_limits<double>::lowest();
    int cntD    = 0;
    double sumF = 0.0;
    int cntF    = 0;
    double sumC = 0.0;
    int cntC    = 0;
    for (const auto& s : out.samples) {
        if (!std::isnan(s.distanceMm)) {
            sumD += s.distanceMm;
            minD = std::min(minD, s.distanceMm);
            maxD = std::max(maxD, s.distanceMm);
            ++cntD;
        }
        if (!std::isnan(s.facingCosine)) {
            sumF += s.facingCosine;
            ++cntF;
        }
        if (!std::isnan(s.congruentMotionCorr)) {
            sumC += s.congruentMotionCorr;
            ++cntC;
        }
    }
    if (cntD > 0) {
        out.stats.meanDistanceMm = sumD / cntD;
        out.stats.minDistanceMm  = minD;
        out.stats.maxDistanceMm  = maxD;
    }
    if (cntF > 0) {
        out.stats.meanFacingCosine = sumF / cntF;
    }
    if (cntC > 0) {
        out.stats.meanCongruentMotionCorr = sumC / cntC;
    }
    out.stats.pctTicksBothPresent = n > 0 ? (100.0 * bothPresentCount / n) : kNaN;

    return out;
}

} // namespace mosaic
