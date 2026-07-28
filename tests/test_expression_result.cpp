#include "analysis/expression_result.hpp"
#include <gtest/gtest.h>
#include <QFile>
#include <QTemporaryDir>

using mosaic::ExpressionResult;

namespace {

// Matches the exact schema written by analysis/run_expression.py's
// _write_results() / _frame_to_dict(). Frames are at index 0, 2, 4 (as if
// written with --skip 2), same convention as test_pose_analysis_result.cpp.
const char* kFixtureJson = R"JSON(
{
  "source_video": "video_0.mp4",
  "backend": "heuristic",
  "blendshape_names": ["_neutral", "mouthSmileLeft", "mouthSmileRight", "browDownLeft"],
  "frames": [
    {
      "frame_index": 0, "timestamp_ns": 1000000000, "camera_index": 0,
      "subjects": [
        {
          "subject_id": 0, "confidence": 1.0,
          "bbox_xyxy": [10.0, 10.0, 100.0, 100.0],
          "blendshape_scores": [0.1, 0.8, 0.75, 0.05],
          "dominant_expression": "Happy", "dominant_score": 0.72
        }
      ]
    },
    {
      "frame_index": 2, "timestamp_ns": 1080000000, "camera_index": 0,
      "subjects": [
        {
          "subject_id": 0, "confidence": 1.0,
          "bbox_xyxy": [12.0, 12.0, 102.0, 102.0],
          "blendshape_scores": [0.85, 0.05, 0.05, 0.02],
          "dominant_expression": "Neutral", "dominant_score": 0.85
        }
      ]
    },
    {
      "frame_index": 4, "timestamp_ns": 1160000000, "camera_index": 0,
      "subjects": []
    }
  ]
}
)JSON";

QString write_fixture(const QString& dirPath) {
    const QString path = dirPath + "/video_0.expression.json";
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(kFixtureJson);
    return path;
}

// Every frame has an empty "subjects" array — a valid result (parses fine)
// but with no face ever detected in this camera's footage, distinguishing
// has_any_detections() from is_valid().
const char* kNoDetectionsFixtureJson = R"JSON(
{
  "source_video": "video_1.mp4",
  "backend": "heuristic",
  "blendshape_names": ["_neutral", "mouthSmileLeft"],
  "frames": [
    { "frame_index": 0, "timestamp_ns": 1000000000, "camera_index": 1, "subjects": [] },
    { "frame_index": 1, "timestamp_ns": 1040000000, "camera_index": 1, "subjects": [] }
  ]
}
)JSON";

QString write_no_detections_fixture(const QString& dirPath) {
    const QString path = dirPath + "/video_1.expression.json";
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(kNoDetectionsFixtureJson);
    return path;
}

} // namespace

TEST(ExpressionResult, LoadsValidFileWithFullSchema) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = write_fixture(dir.path());

    const auto result = ExpressionResult::load(path);

    ASSERT_TRUE(result.is_valid());
    EXPECT_EQ(result.source_video(), "video_0.mp4");
    EXPECT_EQ(result.backend(), "heuristic");
    EXPECT_EQ(result.blendshape_names(),
              QStringList({"_neutral", "mouthSmileLeft", "mouthSmileRight", "browDownLeft"}));

    ASSERT_EQ(result.frames().size(), 3);
    const auto& f0 = result.frames()[0];
    EXPECT_EQ(f0.frameIndex, 0);
    EXPECT_EQ(f0.timestampNs, 1000000000);
    ASSERT_EQ(f0.subjects.size(), 1);
    EXPECT_EQ(f0.subjects[0].subjectId, 0);
    EXPECT_DOUBLE_EQ(f0.subjects[0].confidence, 1.0);
    EXPECT_DOUBLE_EQ(f0.subjects[0].bbox.left(), 10.0);
    ASSERT_EQ(f0.subjects[0].blendshapeScores.size(), 4);
    EXPECT_DOUBLE_EQ(f0.subjects[0].blendshapeScores[1], 0.8);
    EXPECT_EQ(f0.subjects[0].dominantExpression, "Happy");
    EXPECT_DOUBLE_EQ(f0.subjects[0].dominantScore, 0.72);

    // Third frame has no detected faces — must parse to an empty vector,
    // not be dropped or crash.
    EXPECT_TRUE(result.frames()[2].subjects.isEmpty());
}

TEST(ExpressionResult, MissingFileIsInvalidNotCrashing) {
    const auto result = ExpressionResult::load("Z:/does/not/exist.expression.json");
    EXPECT_FALSE(result.is_valid());
    EXPECT_TRUE(result.frames().isEmpty());
}

TEST(ExpressionResult, NearestFrameHandlesGapsAndBoundaries) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = ExpressionResult::load(write_fixture(dir.path()));
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

TEST(ExpressionResult, NearestFrameOnEmptyResultReturnsNull) {
    const ExpressionResult result;
    EXPECT_EQ(result.nearest_frame(0), nullptr);
}

TEST(ExpressionResult, HasAnyDetectionsIsTrueWhenSomeFrameHasASubject) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = ExpressionResult::load(write_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());
    EXPECT_TRUE(result.has_any_detections());
}

TEST(ExpressionResult, HasAnyDetectionsIsFalseWhenNoFrameHasASubject) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = ExpressionResult::load(write_no_detections_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());
    EXPECT_FALSE(result.has_any_detections());
}

TEST(ExpressionResult, HasAnyDetectionsIsFalseOnDefaultConstructedResult) {
    const ExpressionResult result;
    EXPECT_FALSE(result.has_any_detections());
}
