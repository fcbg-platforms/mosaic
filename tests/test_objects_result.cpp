#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include "analysis/objects_result.hpp"

using mosaic::ObjectsResult;

namespace {

// The schema analysis/run_objects.py writes (analyse_camera()), cut down.
const char* kFixtureJson = R"JSON(
{
 "schema": "mosaic-objects-v1",
 "camera_index": 1,
 "labels": ["toy", "cup", "tablet"],
 "video_start_ms": 5000.0,
 "pose_used": true,
 "hands_followed": true,
 "annotated_video": "objects/video_1.objects.mp4",
 "summary": {"objects": [
  {"id": 1, "label": "cup", "seen_pct": 93.3, "travel_px": 40.0, "contacts": 1,
   "touched_s_by_person": {"1": 0.8}, "seen_spans": [[5000.0, 6200.0], [7000.0, 7400.0]]},
  {"id": 2, "label": "toy", "seen_pct": 20.0, "travel_px": 0.0, "contacts": 0,
   "touched_s_by_person": {}}
 ]},
 "contacts": [
  {"object_id": 1, "person": 1, "start_ms": 6000.0, "end_ms": 6800.0},
  {"object_id": 2, "person": null, "start_ms": 7000.0, "end_ms": 7400.0}
 ],
 "frames_file": "objects/video_1.objects.frames.json"
}
)JSON";

ObjectsResult load_fixture(QTemporaryDir& dir, const char* json) {
    const QString path = dir.filePath("video_1.objects.json");
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(json);
    f.close();
    return ObjectsResult::load(path);
}

} // namespace

TEST(ObjectsResult, ReadsObjectsContactsAndSeenSpans) {
    QTemporaryDir dir;
    const ObjectsResult r = load_fixture(dir, kFixtureJson);
    ASSERT_TRUE(r.is_valid());
    EXPECT_EQ(r.labels().size(), 3);
    EXPECT_DOUBLE_EQ(r.video_start_ms(), 5000.0);
    EXPECT_TRUE(r.pose_used());
    EXPECT_TRUE(r.hands_followed());
    EXPECT_EQ(r.annotated_video(), "objects/video_1.objects.mp4");
    ASSERT_EQ(r.objects().size(), 2);
    EXPECT_EQ(r.objects()[0].label, "cup");
    EXPECT_DOUBLE_EQ(r.objects()[0].touchedSByPerson.value("1"), 0.8);
    ASSERT_EQ(r.contacts().size(), 2);
    EXPECT_EQ(r.contacts()[0].person, "1");
    EXPECT_EQ(r.contacts()[1].person, "?"); // an untracked person
    ASSERT_EQ(r.objects()[0].seenSpans.size(), 2);
    EXPECT_DOUBLE_EQ(r.objects()[0].seenSpans[1].first, 7000.0);
    EXPECT_TRUE(r.objects()[1].seenSpans.isEmpty());
}

TEST(ObjectsResult, AnotherSchemaOrNoFileIsInvalid) {
    QTemporaryDir dir;
    EXPECT_FALSE(load_fixture(dir, R"({"schema": "mosaic-social-v1"})").is_valid());
    // A result from before hands were followed still loads.
    const ObjectsResult old = load_fixture(dir, R"({"schema": "mosaic-objects-v1"})");
    EXPECT_TRUE(old.is_valid());
    EXPECT_FALSE(old.hands_followed());
    EXPECT_FALSE(ObjectsResult::load(dir.filePath("missing.json")).is_valid());
}
