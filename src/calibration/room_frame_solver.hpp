#pragma once
#include <array>
#include <vector>

namespace mosaic::room_frame {

/// @brief 4×4 homogeneous rigid transform, row-major — same convention as
/// CalibrationData::extrinsicRt (src/core/settings.hpp): index i*4+j is
/// row i, column j; indices 3/7/11 are the translation; bottom row is
/// implicitly [0,0,0,1] for every value this module produces or consumes.
using Mat4 = std::array<double, 16>;

/// @brief Returns the 4×4 identity transform.
[[nodiscard]] Mat4 identity();

/// @brief Rigid-transform inverse: for m = [R|t], returns [R^T | -R^T t].
[[nodiscard]] Mat4 invert(const Mat4& m);

/// @brief a ∘ b — applies b first, then a (standard 4×4 matrix product a*b).
[[nodiscard]] Mat4 compose(const Mat4& a, const Mat4& b);

/// @brief Quaternion-mean rotation (sign-aligned to the first sample, then
/// renormalized) + arithmetic-mean translation.
///
/// Only a valid approximation of the "correct" rotation average
/// (chordal/Karcher mean) when the input rotations are already close
/// together — true here because samples are repeated shots of the same
/// static rig. Returns identity() for an empty input, echoes the single
/// sample unchanged for a size-1 input.
///
/// @see docs/math/room_calibration.rst for the full derivation.
[[nodiscard]] Mat4 average(const std::vector<Mat4>& samples);

/// @brief One ChArUco-board shot seen simultaneously by two cameras (camA, camB).
///
/// boardToCamA[k]/boardToCamB[k] are the board→camera pose pair from the
/// same shot k (as produced by cv::solvePnP), so the two vectors must be
/// the same length and index-aligned.
struct Edge {
    int              camA = -1;   ///< First camera's index.
    int              camB = -1;   ///< Second camera's index.
    std::vector<Mat4> boardToCamA; ///< board→camA pose per shared shot.
    std::vector<Mat4> boardToCamB; ///< board→camB pose per shared shot, index-aligned with boardToCamA.
};

/// @brief Output of bfs_resolve() — one entry per camera, indexed by camera index.
struct ResolveResult {
    std::vector<bool> resolved;      ///< true if this camera's pose was resolved.
    std::vector<Mat4> extrinsicRt;   ///< pose relative to referenceIndex; identity() where !resolved[i]
};

/// @brief Resolves every camera's extrinsicRt (pose relative to referenceIndex) by
/// BFS over the shared-shot graph described by edges.
///
/// referenceIndex always resolves to identity() (matching
/// CalibrationData::extrinsicRt's own documented "camera 0 always has
/// identity" convention). A camera with no shot-chain back to
/// referenceIndex is reported unresolved rather than silently left with a
/// meaningless pose.
///
/// @see docs/math/room_calibration.rst for the full derivation.
[[nodiscard]] ResolveResult bfs_resolve(int cameraCount, int referenceIndex,
                                         const std::vector<Edge>& edges);

} // namespace mosaic::room_frame
