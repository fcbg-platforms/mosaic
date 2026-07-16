#include "analysis/pose_analysis_result.hpp"
#include <gtest/gtest.h>
#include <QFile>
#include <QTemporaryDir>

using mosaic::PoseAnalysisResult;

namespace {

// Matches the exact schema written by analysis/run_pose.py's
// _write_results() / _result_to_dict(), including skeleton_edges.
// Frames are at index 0, 2, 4 (as if written with --skip 2).
const char* kFixtureJson = R"JSON(
{
  "source_video": "video_0.mp4",
  "keypoint_names": ["nose", "left_eye", "right_eye"],
  "skeleton_edges": [[0, 1], [0, 2]],
  "frames": [
    {
      "frame_index": 0, "timestamp_ns": 1000000000, "camera_index": 0,
      "inference_ms": 5.0, "backend": "yolov8",
      "subjects": [
        {
          "subject_id": 0, "confidence": 0.9,
          "bbox_xyxy": [10.0, 10.0, 100.0, 100.0],
          "keypoints": [[50.0, 60.0], [45.0, 55.0], [55.0, 55.0]],
          "visibilities": [0.95, 0.9, 0.9]
        }
      ]
    },
    {
      "frame_index": 2, "timestamp_ns": 1080000000, "camera_index": 0,
      "inference_ms": 5.0, "backend": "yolov8",
      "subjects": [
        {
          "subject_id": 0, "confidence": 0.9,
          "bbox_xyxy": [12.0, 12.0, 102.0, 102.0],
          "keypoints": [[52.0, 62.0], [47.0, 57.0], [57.0, 57.0]],
          "visibilities": [0.95, 0.9, 0.9]
        }
      ]
    },
    {
      "frame_index": 4, "timestamp_ns": 1160000000, "camera_index": 0,
      "inference_ms": 5.0, "backend": "yolov8",
      "subjects": []
    }
  ]
}
)JSON";

QString write_fixture(const QString& dirPath) {
    const QString path = dirPath + "/video_0.pose.json";
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(kFixtureJson);
    return path;
}

} // namespace

TEST(PoseAnalysisResult, LoadsValidFileWithFullSchema) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = write_fixture(dir.path());

    const auto result = PoseAnalysisResult::load(path);

    ASSERT_TRUE(result.is_valid());
    EXPECT_EQ(result.source_video(), "video_0.mp4");
    EXPECT_EQ(result.keypoint_names(), QStringList({"nose", "left_eye", "right_eye"}));
    ASSERT_EQ(result.skeleton_edges().size(), 2);
    EXPECT_EQ(result.skeleton_edges()[0], qMakePair(0, 1));
    EXPECT_EQ(result.skeleton_edges()[1], qMakePair(0, 2));

    ASSERT_EQ(result.frames().size(), 3);
    const auto& f0 = result.frames()[0];
    EXPECT_EQ(f0.frameIndex, 0);
    EXPECT_EQ(f0.timestampNs, 1000000000);
    ASSERT_EQ(f0.subjects.size(), 1);
    EXPECT_EQ(f0.subjects[0].subjectId, 0);
    EXPECT_DOUBLE_EQ(f0.subjects[0].confidence, 0.9);
    ASSERT_EQ(f0.subjects[0].keypoints.size(), 3);
    EXPECT_DOUBLE_EQ(f0.subjects[0].keypoints[0].x(), 50.0);
    EXPECT_DOUBLE_EQ(f0.subjects[0].keypoints[0].y(), 60.0);
    EXPECT_DOUBLE_EQ(f0.subjects[0].bbox.left(), 10.0);

    // Third frame has no detected subjects — must parse to an empty vector,
    // not be dropped or crash.
    EXPECT_TRUE(result.frames()[2].subjects.isEmpty());
}

TEST(PoseAnalysisResult, MissingFileIsInvalidNotCrashing) {
    const auto result = PoseAnalysisResult::load("Z:/does/not/exist.pose.json");
    EXPECT_FALSE(result.is_valid());
    EXPECT_TRUE(result.frames().isEmpty());
}

TEST(PoseAnalysisResult, NearestFrameHandlesGapsAndBoundaries) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = PoseAnalysisResult::load(write_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());

    // Exact matches.
    EXPECT_EQ(result.nearest_frame(0)->frameIndex, 0);
    EXPECT_EQ(result.nearest_frame(2)->frameIndex, 2);
    EXPECT_EQ(result.nearest_frame(4)->frameIndex, 4);

    // Before the first frame clamps to the first frame.
    EXPECT_EQ(result.nearest_frame(-5)->frameIndex, 0);

    // After the last frame clamps to the last frame.
    EXPECT_EQ(result.nearest_frame(100)->frameIndex, 4);

    // Mid-gap ties resolve to the earlier frame (matches the
    // "beforeDelta <= afterDelta" tie-break) — both 1 (between 0 and 2)
    // and 3 (between 2 and 4) are exact ties.
    EXPECT_EQ(result.nearest_frame(1)->frameIndex, 0);
    EXPECT_EQ(result.nearest_frame(3)->frameIndex, 2);
}

TEST(PoseAnalysisResult, NearestFrameOnEmptyResultReturnsNull) {
    const PoseAnalysisResult result;
    EXPECT_EQ(result.nearest_frame(0), nullptr);
}
