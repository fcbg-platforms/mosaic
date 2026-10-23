#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "analysis/skeleton3d_result.hpp"

namespace mosaic::room_view {

// Which way is up, and where the floor is, for the 3D room view.
//
// The room frame is the reference camera's own (x right, y down, z forward,
// millimetres; docs/math/room_calibration.rst), so "up" is only -y when
// that camera is level, and the floor is wherever the people stand. Both
// are read from the data rather than assumed, so people are drawn upright
// on a floor however the reference camera was mounted.

using Vec3 = std::array<double, 3>;

inline Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Vec3 scale(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
inline double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }
inline Vec3 unit(const Vec3& a) {
    const double n = norm(a);
    return n > 1e-12 ? scale(a, 1.0 / n) : Vec3{0, 0, 0};
}

/// People seen standing or sitting upright: at least this many shoulder-over-
/// hip samples before they decide which way is up.
inline constexpr int kMinUprightSamples = 10;

/// The room's up direction, a unit vector in the room frame.
///
/// From the people: the mean direction from the middle of the hips to the
/// middle of the shoulders, over every person and frame that has all four
/// (a torso is upright far more often than not, sitting or standing).
/// Without enough of those, from the cameras: the mean of each camera's own
/// image-up direction (minus its y axis), since room cameras are mounted
/// roughly level. Without either, -y, the reference camera's image-up.
inline Vec3 estimate_up(const Skeleton3DResult& r) {
    const auto& names = r.keypoint_names();
    const int ls      = static_cast<int>(names.indexOf("left_shoulder"));
    const int rs      = static_cast<int>(names.indexOf("right_shoulder"));
    const int lh      = static_cast<int>(names.indexOf("left_hip"));
    const int rh      = static_cast<int>(names.indexOf("right_hip"));
    Vec3 sum{0, 0, 0};
    int n = 0;
    if (ls >= 0 && rs >= 0 && lh >= 0 && rh >= 0) {
        for (const auto& f : r.frames()) {
            for (const auto& p : f.people) {
                const auto& k  = p.keypoints;
                const int need = std::max({ls, rs, lh, rh});
                if (need >= k.size() || !k[ls].valid || !k[rs].valid || !k[lh].valid ||
                    !k[rh].valid) {
                    continue;
                }
                const Vec3 shoulders = scale(add(k[ls].positionRoom, k[rs].positionRoom), 0.5);
                const Vec3 hips      = scale(add(k[lh].positionRoom, k[rh].positionRoom), 0.5);
                sum                  = add(sum, unit(sub(shoulders, hips)));
                ++n;
            }
        }
    }
    // A mean of unit vectors this short means torsos point every which way
    // (people lying down, or bad triangulation): not a direction to trust.
    if (n >= kMinUprightSamples && norm(sum) > 0.5 * n) {
        return unit(sum);
    }
    Vec3 camUp{0, 0, 0};
    for (const auto& c : r.cameras()) {
        if (c.hasRotation) {
            // The camera's y axis in the room is the rotation's second column.
            const auto& m = c.rotationRoom;
            camUp         = sub(camUp, Vec3{m[1], m[4], m[7]});
        }
    }
    if (norm(camUp) > 1e-9) {
        return unit(camUp);
    }
    return {0, -1, 0};
}

/// The floor's height along `up` (mm, as dot(point, up)).
///
/// From the ankles: the 5th percentile of their heights, less the ankle's
/// own height above the floor (about 80 mm), so a few low outliers do not
/// sink the floor. Without ankles, the lowest keypoint seen; without any,
/// 2 m below the lowest camera (room cameras hang at about head height).
inline double floor_height(const Skeleton3DResult& r, const Vec3& up) {
    const auto& names = r.keypoint_names();
    const int la      = static_cast<int>(names.indexOf("left_ankle"));
    const int ra      = static_cast<int>(names.indexOf("right_ankle"));
    std::vector<double> ankles;
    double lowest = 0.0;
    bool any      = false;
    for (const auto& f : r.frames()) {
        for (const auto& p : f.people) {
            for (int i = 0; i < p.keypoints.size(); ++i) {
                if (!p.keypoints[i].valid) {
                    continue;
                }
                const double h = dot(p.keypoints[i].positionRoom, up);
                lowest         = any ? std::min(lowest, h) : h;
                any            = true;
                if (i == la || i == ra) {
                    ankles.push_back(h);
                }
            }
        }
    }
    if (!ankles.empty()) {
        std::sort(ankles.begin(), ankles.end());
        return ankles[ankles.size() * 5 / 100] - 80.0;
    }
    if (any) {
        return lowest;
    }
    double camLow = 0.0;
    bool anyCam   = false;
    for (const auto& c : r.cameras()) {
        const double h = dot(c.positionRoom, up);
        camLow         = anyCam ? std::min(camLow, h) : h;
        anyCam         = true;
    }
    return camLow - 2000.0;
}

/// The view's "forward": the reference camera's viewing direction (room +z)
/// laid flat on the floor, so the Front view looks the way that camera does.
/// Falls back to room +x when that camera looks straight up or down.
inline Vec3 floor_forward(const Vec3& up) {
    const Vec3 z{0, 0, 1};
    const Vec3 flat = sub(z, scale(up, dot(z, up)));
    if (norm(flat) > 0.2) {
        return unit(flat);
    }
    const Vec3 x{1, 0, 0};
    return unit(sub(x, scale(up, dot(x, up))));
}

} // namespace mosaic::room_view
