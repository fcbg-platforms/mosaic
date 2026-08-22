#include "analysis/dyadic_kinematics.hpp"

#include <QList>
#include <QMap>
#include <algorithm>
#include <cmath>
#include <optional>

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

} // namespace

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
