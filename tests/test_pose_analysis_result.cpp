#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include "analysis/pose_analysis_result.hpp"

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

// Same schema, but every frame has zero detected subjects — models a
// camera whose footage the pose model never detected anyone in for the
// whole session (a real, confirmed-on-hardware case, not hypothetical).
const char* kNoDetectionsFixtureJson = R"JSON(
{
  "source_video": "video_0.mp4",
  "keypoint_names": ["nose", "left_eye", "right_eye"],
  "skeleton_edges": [[0, 1], [0, 2]],
  "frames": [
    { "frame_index": 0, "timestamp_ns": 1000000000, "camera_index": 0,
      "inference_ms": 5.0, "backend": "yolov8", "subjects": [] },
    { "frame_index": 1, "timestamp_ns": 1040000000, "camera_index": 0,
      "inference_ms": 5.0, "backend": "yolov8", "subjects": [] }
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

QString write_no_detections_fixture(const QString& dirPath) {
    const QString path = dirPath + "/video_0.pose.json";
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(kNoDetectionsFixtureJson);
    return path;
}

// Same schema as kFixtureJson, plus the top-level "model" field added when
// Pose output was namespaced by model (see AnalysisTabW::slug_for_model()).
const char* kWithModelFixtureJson = R"JSON(
{
  "source_video": "video_0.mp4",
  "model": "yolov8n-pose.pt",
  "keypoint_names": ["nose", "left_eye", "right_eye"],
  "skeleton_edges": [[0, 1], [0, 2]],
  "frames": []
}
)JSON";

QString write_with_model_fixture(const QString& dirPath) {
    const QString path = dirPath + "/video_0.yolov8n-pose.pose.json";
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(kWithModelFixtureJson);
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

TEST(PoseAnalysisResult, HasAnyDetectionsIsTrueWhenSomeFrameHasASubject) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = PoseAnalysisResult::load(write_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());
    EXPECT_TRUE(result.has_any_detections());
}

TEST(PoseAnalysisResult, HasAnyDetectionsIsFalseWhenNoFrameHasASubject) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = PoseAnalysisResult::load(write_no_detections_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());
    EXPECT_FALSE(result.has_any_detections());
}

TEST(PoseAnalysisResult, HasAnyDetectionsIsFalseOnDefaultConstructedResult) {
    const PoseAnalysisResult result;
    EXPECT_FALSE(result.has_any_detections());
}

TEST(PoseAnalysisResult, ModelIsEmptyForOlderFilesWithNoModelField) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = PoseAnalysisResult::load(write_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());
    EXPECT_TRUE(result.model().isEmpty());
}

TEST(PoseAnalysisResult, ModelParsesFromTopLevelField) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = PoseAnalysisResult::load(write_with_model_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());
    EXPECT_EQ(result.model(), "yolov8n-pose.pt");
}

// ── Subject identity ───────────────────────────────────────────────────────

namespace {

// Writes a .pose.json with an arbitrary top-level body, for the identity
// tests below — each needs its own subject-id shape, so the shared fixtures
// above don't fit.
QString write_json(const QString& dirPath, const QString& json) {
    const QString path = dirPath + "/video_0.pose.json";
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(json.toUtf8());
    return path;
}

// One frame carrying `subjects` verbatim, so a test can shape ids freely.
QString frame_with(int frameIndex, const QString& subjects) {
    return QString(R"({"frame_index": %1, "timestamp_ns": %2, "camera_index": 0,
        "inference_ms": 1.0, "backend": "test", "subjects": [%3]})")
        .arg(frameIndex)
        .arg(static_cast<qint64>(frameIndex) * 1000000000LL)
        .arg(subjects);
}

QString subject_with_id(const QString& idField) {
    return QString(R"({%1 "confidence": 0.9, "bbox_xyxy": [0,0,1,1],
        "keypoints": [[1.0, 2.0]], "visibilities": [0.9]})")
        .arg(idField);
}

QString doc_with(const QString& frames, const QString& extraTopLevel = QString()) {
    return QString(R"({"source_video": "video_0.mp4", %1
        "keypoint_names": ["nose"], "skeleton_edges": [], "frames": [%2]})")
        .arg(extraTopLevel, frames);
}

} // namespace

TEST(PoseAnalysisResultSubjects, LegacyDenseIdsReproduceTheOldChipOrder) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString frames = frame_with(
        0, subject_with_id(R"("subject_id": 0,)") + "," + subject_with_id(R"("subject_id": 1,)"));
    const auto result = PoseAnalysisResult::load(write_json(dir.path(), doc_with(frames)));
    ASSERT_TRUE(result.is_valid());

    ASSERT_EQ(result.subject_ids().size(), 2);
    EXPECT_EQ(result.subject_ids()[0].value, 0);
    EXPECT_EQ(result.subject_ids()[1].value, 1);
}

TEST(PoseAnalysisResultSubjects, IdsAreDedupedAndSortedAcrossFrames) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // Deliberately out of order and repeated, the way a tracker's ids appear
    // as people enter and leave.
    const QString frames = frame_with(0, subject_with_id(R"("subject_id": 2,)")) + "," +
                           frame_with(1, subject_with_id(R"("subject_id": 1,)") + "," +
                                             subject_with_id(R"("subject_id": 2,)")) +
                           "," + frame_with(2, subject_with_id(R"("subject_id": 1,)"));
    const auto result = PoseAnalysisResult::load(write_json(dir.path(), doc_with(frames)));
    ASSERT_TRUE(result.is_valid());

    ASSERT_EQ(result.subject_ids().size(), 2);
    EXPECT_EQ(result.subject_ids()[0].value, 1);
    EXPECT_EQ(result.subject_ids()[1].value, 2);
}

// Untracked ids are assigned per frame by run_pose.py (-(i+1)), so the "-1" in
// one frame is a different person from the "-1" in the next. Aggregating them
// would splice unrelated people into a single trajectory — the exact bug that
// identity keying exists to remove — so they must never reach the chips, the
// chart or the export. They stay visible on the video overlay, where each
// frame stands alone and no cross-frame claim is made.
TEST(PoseAnalysisResultSubjects, UntrackedIdsAreExcludedFromCrossFrameAggregation) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString frames = frame_with(0, subject_with_id(R"("subject_id": -2,)") + "," +
                                             subject_with_id(R"("subject_id": 1,)") + "," +
                                             subject_with_id(R"("subject_id": -1,)"));
    const auto result    = PoseAnalysisResult::load(write_json(dir.path(), doc_with(frames)));
    ASSERT_TRUE(result.is_valid());

    ASSERT_EQ(result.subject_ids().size(), 1);
    EXPECT_EQ(result.subject_ids()[0].value, 1);
    EXPECT_TRUE(result.has_untracked_detections());
    // Still parsed and present in the frame — the overlay draws them.
    EXPECT_EQ(result.frames()[0].subjects.size(), 3);
}

TEST(PoseAnalysisResultSubjects, NoUntrackedFlagWhenEveryDetectionIsTracked) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = PoseAnalysisResult::load(write_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());
    EXPECT_FALSE(result.has_untracked_detections());
}

// The silent-wrong-person guard. QJsonValue::toInt() returns 0 for a missing
// key, which under identity keying would collapse every subject in the frame
// onto id 0 — and a lookup would then quietly return whichever person was
// listed first, rather than failing.
TEST(PoseAnalysisResultSubjects, AbsentSubjectIdFallsBackToArrayPositionNotZero) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString frames =
        frame_with(0, subject_with_id("") + "," + subject_with_id("") + "," + subject_with_id(""));
    const auto result = PoseAnalysisResult::load(write_json(dir.path(), doc_with(frames)));
    ASSERT_TRUE(result.is_valid());

    ASSERT_EQ(result.frames().size(), 1);
    const auto& subjects = result.frames()[0].subjects;
    ASSERT_EQ(subjects.size(), 3);
    EXPECT_EQ(subjects[0].subjectId, 0);
    EXPECT_EQ(subjects[1].subjectId, 1);
    EXPECT_EQ(subjects[2].subjectId, 2);
    EXPECT_EQ(result.subject_ids().size(), 3);
}

TEST(PoseAnalysisResultSubjects, TrackerIsEmptyForPreTrackingFiles) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = PoseAnalysisResult::load(write_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());
    EXPECT_TRUE(result.tracker().isEmpty());
    EXPECT_FALSE(result.has_tracked_identity());
}

TEST(PoseAnalysisResultSubjects, TrackerParsesFromTopLevelField) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString frames = frame_with(0, subject_with_id(R"("subject_id": 1,)"));
    const auto result    = PoseAnalysisResult::load(
        write_json(dir.path(), doc_with(frames, R"("tracker": "botsort",)")));
    ASSERT_TRUE(result.is_valid());
    EXPECT_EQ(result.tracker(), "botsort");
    EXPECT_TRUE(result.has_tracked_identity());
}

TEST(PoseAnalysisResultSubjects, FindSubjectReturnsNullWhenThatPersonIsAbsent) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString frames = frame_with(0, subject_with_id(R"("subject_id": 7,)"));
    const auto result    = PoseAnalysisResult::load(write_json(dir.path(), doc_with(frames)));
    ASSERT_TRUE(result.is_valid());
    ASSERT_EQ(result.frames().size(), 1);

    EXPECT_NE(mosaic::find_subject(result.frames()[0], mosaic::SubjectId(7)), nullptr);
    // Not "index 0 of a one-subject frame" — genuinely absent.
    EXPECT_EQ(mosaic::find_subject(result.frames()[0], mosaic::SubjectId(0)), nullptr);
}
