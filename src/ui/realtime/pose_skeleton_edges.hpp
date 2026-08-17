#pragma once

namespace mosaic {

// Bone-connection table for MediaPipe's standard 33-point pose landmark set
// (see python/pose/estimator.py's _LANDMARK_NAMES for the exact name list
// the live pose_ready() JSON carries — this table is matched against those
// names, not indices, since the live protocol doesn't send an edge list the
// way the offline run_pose.py's .pose.json does). Mirrors MediaPipe's own
// POSE_CONNECTIONS constant, trimmed to the subset useful for a small live
// tile overlay (torso/limbs; drops the fine-grained face mesh connections
// around eyes/mouth that are illegible at tile size).
struct PoseNameEdge {
    const char* a;
    const char* b;
};

inline constexpr PoseNameEdge kPoseNameEdges[] = {
    // Face
    {"NOSE", "LEFT_EYE_INNER"},
    {"LEFT_EYE_INNER", "LEFT_EYE"},
    {"LEFT_EYE", "LEFT_EYE_OUTER"},
    {"LEFT_EYE_OUTER", "LEFT_EAR"},
    {"NOSE", "RIGHT_EYE_INNER"},
    {"RIGHT_EYE_INNER", "RIGHT_EYE"},
    {"RIGHT_EYE", "RIGHT_EYE_OUTER"},
    {"RIGHT_EYE_OUTER", "RIGHT_EAR"},
    {"MOUTH_LEFT", "MOUTH_RIGHT"},

    // Torso
    {"LEFT_SHOULDER", "RIGHT_SHOULDER"},
    {"LEFT_SHOULDER", "LEFT_HIP"},
    {"RIGHT_SHOULDER", "RIGHT_HIP"},
    {"LEFT_HIP", "RIGHT_HIP"},

    // Left arm
    {"LEFT_SHOULDER", "LEFT_ELBOW"},
    {"LEFT_ELBOW", "LEFT_WRIST"},
    {"LEFT_WRIST", "LEFT_PINKY"},
    {"LEFT_WRIST", "LEFT_INDEX"},
    {"LEFT_WRIST", "LEFT_THUMB"},
    {"LEFT_PINKY", "LEFT_INDEX"},

    // Right arm
    {"RIGHT_SHOULDER", "RIGHT_ELBOW"},
    {"RIGHT_ELBOW", "RIGHT_WRIST"},
    {"RIGHT_WRIST", "RIGHT_PINKY"},
    {"RIGHT_WRIST", "RIGHT_INDEX"},
    {"RIGHT_WRIST", "RIGHT_THUMB"},
    {"RIGHT_PINKY", "RIGHT_INDEX"},

    // Left leg
    {"LEFT_HIP", "LEFT_KNEE"},
    {"LEFT_KNEE", "LEFT_ANKLE"},
    {"LEFT_ANKLE", "LEFT_HEEL"},
    {"LEFT_HEEL", "LEFT_FOOT_INDEX"},
    {"LEFT_ANKLE", "LEFT_FOOT_INDEX"},

    // Right leg
    {"RIGHT_HIP", "RIGHT_KNEE"},
    {"RIGHT_KNEE", "RIGHT_ANKLE"},
    {"RIGHT_ANKLE", "RIGHT_HEEL"},
    {"RIGHT_HEEL", "RIGHT_FOOT_INDEX"},
    {"RIGHT_ANKLE", "RIGHT_FOOT_INDEX"},
};

inline constexpr int kPoseNameEdgeCount =
    static_cast<int>(sizeof(kPoseNameEdges) / sizeof(kPoseNameEdges[0]));

} // namespace mosaic
