#include "calibration/room_frame_solver.hpp"
#include <cmath>
#include <queue>

namespace mosaic::room_frame {

namespace {

struct Quat {
    double w = 1.0, x = 0.0, y = 0.0, z = 0.0;
};

// Robust rotation-matrix -> quaternion conversion (Shepperd's method).
// `r` is the 3×3 rotation part in row-major order: r[0..2]=row0, etc.
Quat mat_to_quat(const std::array<double, 9>& r) {
    Quat q;
    const double trace = r[0] + r[4] + r[8];
    if (trace > 0.0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;   // s = 4*qw
        q.w = 0.25 * s;
        q.x = (r[7] - r[5]) / s;
        q.y = (r[2] - r[6]) / s;
        q.z = (r[3] - r[1]) / s;
    } else if (r[0] > r[4] && r[0] > r[8]) {
        const double s = std::sqrt(1.0 + r[0] - r[4] - r[8]) * 2.0;   // s = 4*qx
        q.w = (r[7] - r[5]) / s;
        q.x = 0.25 * s;
        q.y = (r[1] + r[3]) / s;
        q.z = (r[2] + r[6]) / s;
    } else if (r[4] > r[8]) {
        const double s = std::sqrt(1.0 + r[4] - r[0] - r[8]) * 2.0;   // s = 4*qy
        q.w = (r[2] - r[6]) / s;
        q.x = (r[1] + r[3]) / s;
        q.y = 0.25 * s;
        q.z = (r[5] + r[7]) / s;
    } else {
        const double s = std::sqrt(1.0 + r[8] - r[0] - r[4]) * 2.0;   // s = 4*qz
        q.w = (r[3] - r[1]) / s;
        q.x = (r[2] + r[6]) / s;
        q.y = (r[5] + r[7]) / s;
        q.z = 0.25 * s;
    }
    return q;
}

// Quaternion (assumed normalized) -> 3×3 rotation matrix, row-major.
std::array<double, 9> quat_to_mat(const Quat& q) {
    const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    return {
        1.0 - 2.0 * (yy + zz), 2.0 * (xy - wz),       2.0 * (xz + wy),
        2.0 * (xy + wz),       1.0 - 2.0 * (xx + zz), 2.0 * (yz - wx),
        2.0 * (xz - wy),       2.0 * (yz + wx),       1.0 - 2.0 * (xx + yy),
    };
}

} // namespace

Mat4 identity() {
    return {1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1};
}

Mat4 invert(const Mat4& m) {
    Mat4 r = identity();
    // Transpose the rotation part.
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r[i * 4 + j] = m[j * 4 + i];
        }
    }
    // new_t = -R^T * t
    const double tx = m[3], ty = m[7], tz = m[11];
    r[3]  = -(r[0] * tx + r[1] * ty + r[2] * tz);
    r[7]  = -(r[4] * tx + r[5] * ty + r[6] * tz);
    r[11] = -(r[8] * tx + r[9] * ty + r[10] * tz);
    return r;
}

Mat4 compose(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += a[i * 4 + k] * b[k * 4 + j];
            }
            r[i * 4 + j] = sum;
        }
    }
    return r;
}

Mat4 average(const std::vector<Mat4>& samples) {
    if (samples.empty()) {
        return identity();
    }
    if (samples.size() == 1) {
        return samples[0];
    }

    std::vector<Quat> quats;
    quats.reserve(samples.size());
    double tSumX = 0.0, tSumY = 0.0, tSumZ = 0.0;
    for (const auto& m : samples) {
        const std::array<double, 9> rot = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
        quats.push_back(mat_to_quat(rot));
        tSumX += m[3];
        tSumY += m[7];
        tSumZ += m[11];
    }

    // Sign-align every quaternion to the first sample before summing —
    // q and -q represent the same rotation, and a naive arithmetic mean
    // without this alignment can cancel out instead of averaging.
    const Quat& ref = quats.front();
    double sw = 0.0, sx = 0.0, sy = 0.0, sz = 0.0;
    for (const auto& q : quats) {
        const double dot = ref.w * q.w + ref.x * q.x + ref.y * q.y + ref.z * q.z;
        const double sign = (dot < 0.0) ? -1.0 : 1.0;
        sw += sign * q.w;
        sx += sign * q.x;
        sy += sign * q.y;
        sz += sign * q.z;
    }
    const double norm = std::sqrt(sw * sw + sx * sx + sy * sy + sz * sz);
    const Quat avgQ = (norm > 1e-12)
        ? Quat{sw / norm, sx / norm, sy / norm, sz / norm}
        : ref;

    const auto rot = quat_to_mat(avgQ);
    const double n = static_cast<double>(samples.size());
    Mat4 result = identity();
    result[0] = rot[0]; result[1] = rot[1]; result[2]  = rot[2];
    result[4] = rot[3]; result[5] = rot[4]; result[6]  = rot[5];
    result[8] = rot[6]; result[9] = rot[7]; result[10] = rot[8];
    result[3]  = tSumX / n;
    result[7]  = tSumY / n;
    result[11] = tSumZ / n;
    return result;
}

ResolveResult bfs_resolve(int cameraCount, int referenceIndex, const std::vector<Edge>& edges) {
    ResolveResult res;
    res.resolved.assign(static_cast<size_t>(cameraCount), false);
    res.extrinsicRt.assign(static_cast<size_t>(cameraCount), identity());

    if (referenceIndex < 0 || referenceIndex >= cameraCount) {
        return res;
    }

    struct AdjEntry {
        int         neighbor;
        const Edge* edge;
        bool        selfIsA;   // true if the traversing side of this edge is camA
    };
    std::vector<std::vector<AdjEntry>> adj(static_cast<size_t>(cameraCount));
    for (const auto& e : edges) {
        if (e.camA < 0 || e.camA >= cameraCount || e.camB < 0 || e.camB >= cameraCount) {
            continue;
        }
        if (e.boardToCamA.empty() || e.boardToCamA.size() != e.boardToCamB.size()) {
            continue;
        }
        adj[static_cast<size_t>(e.camA)].push_back({e.camB, &e, true});
        adj[static_cast<size_t>(e.camB)].push_back({e.camA, &e, false});
    }

    std::queue<int> q;
    res.resolved[static_cast<size_t>(referenceIndex)] = true;
    q.push(referenceIndex);

    while (!q.empty()) {
        const int parent = q.front();
        q.pop();

        for (const auto& adjEntry : adj[static_cast<size_t>(parent)]) {
            const int child = adjEntry.neighbor;
            if (res.resolved[static_cast<size_t>(child)]) {
                continue;
            }

            const Edge& e = *adjEntry.edge;
            std::vector<Mat4> candidates;
            candidates.reserve(e.boardToCamA.size());
            for (size_t k = 0; k < e.boardToCamA.size(); ++k) {
                const Mat4& boardToParent = adjEntry.selfIsA ? e.boardToCamA[k] : e.boardToCamB[k];
                const Mat4& boardToChild  = adjEntry.selfIsA ? e.boardToCamB[k] : e.boardToCamA[k];
                candidates.push_back(
                    compose(compose(res.extrinsicRt[static_cast<size_t>(parent)], boardToParent),
                            invert(boardToChild)));
            }

            res.extrinsicRt[static_cast<size_t>(child)] = average(candidates);
            res.resolved[static_cast<size_t>(child)]    = true;
            q.push(child);
        }
    }

    return res;
}

} // namespace mosaic::room_frame
