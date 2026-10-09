#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <cmath>

#include "analysis/eye_contact_result.hpp"

using mosaic::EyeContactResult;

namespace {

// The schema analysis/run_eye_contact.py writes (write_outputs()), cut down.
const char* kFixtureJson = R"JSON(
{
 "schema": "mosaic-eye-contact-v1",
 "source_video": "video_2.mp4",
 "camera_index": 2,
 "fps": 50.0,
 "annotated_video": "eye_contact/video_2.eye_contact.mp4",
 "target": {"method": "auto", "from": "listening", "yaw": 24.8, "pitch": -5.1,
            "radius_deg": 6.4, "spread_deg": 2.5, "share_inside_pct": 71.0},
 "summary": {
  "eye_contact_pct": 64.2, "aversions": 12, "aversions_per_min": 6.0,
  "aversion_median_s": 2.9,
  "aversion_directions": {"up": 9, "down": 1, "left": 2, "right": 0},
  "eye_contact_speaking_pct": 29.4, "eye_contact_listening_pct": 98.8,
  "eye_contact_silence_pct": null,
  "turns": {"subject_turns": 6, "turn_start_aversion_pct": 100.0,
            "turn_end_contact_pct": null, "response_gaps": 0, "gap_aversion_pct": null}
 },
 "aversions": [
  {"start_ms": 1030000, "end_ms": 1033000, "duration_ms": 3000, "offset_deg": 25.4, "direction": "up"},
  {"start_ms": 1010000, "end_ms": 1013000, "duration_ms": 3000, "offset_deg": 24.9, "direction": "left"}
 ],
 "frames": [
  {"frame_index": 0, "timestamp_ms": 1000000, "face_detected": true,
   "gaze_yaw": 24.1, "gaze_pitch": -4.0, "offset_deg": 1.3, "contact": true},
  {"frame_index": 1, "timestamp_ms": 1000020, "face_detected": true, "contact": null},
  {"frame_index": 2, "timestamp_ms": 1000040, "face_detected": true,
   "gaze_yaw": 5.0, "gaze_pitch": 20.0, "offset_deg": 30.2, "contact": false}
 ]
}
)JSON";

QString write_file(const QString& dir, const QByteArray& content) {
    const QString path = dir + "/video_2.eye_contact.json";
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(content);
    return path;
}

} // namespace

TEST(EyeContactResult, LoadsTargetFramesAversionsAndSummary) {
    QTemporaryDir dir;
    const auto r = EyeContactResult::load(write_file(dir.path(), kFixtureJson));
    ASSERT_TRUE(r.is_valid());
    EXPECT_EQ(r.annotated_video(), "eye_contact/video_2.eye_contact.mp4");
    EXPECT_EQ(r.target_method(), "auto");
    ASSERT_TRUE(r.target_yaw().has_value());
    EXPECT_DOUBLE_EQ(*r.target_yaw(), 24.8);
    ASSERT_TRUE(r.radius_deg().has_value());
    EXPECT_DOUBLE_EQ(*r.radius_deg(), 6.4);

    ASSERT_EQ(r.frames().size(), 3);
    EXPECT_EQ(r.frames()[0].contact, 1);
    EXPECT_EQ(r.frames()[1].contact, -1);
    EXPECT_TRUE(std::isnan(r.frames()[1].gazeYaw));
    EXPECT_EQ(r.frames()[2].contact, 0);
    EXPECT_DOUBLE_EQ(r.frames()[2].offsetDeg, 30.2);

    ASSERT_EQ(r.aversions().size(), 2);
    EXPECT_EQ(r.aversions()[0].direction, "left"); // sorted by start
    EXPECT_EQ(r.aversions()[1].durationMs, 3000);

    const auto& s = r.summary();
    ASSERT_TRUE(s.eyeContactPct.has_value());
    EXPECT_DOUBLE_EQ(*s.eyeContactPct, 64.2);
    EXPECT_DOUBLE_EQ(*s.listeningPct, 98.8);
    EXPECT_EQ(s.aversions, 12);
    EXPECT_EQ(s.up, 9);
    EXPECT_EQ(s.left, 2);
    EXPECT_EQ(s.subjectTurns, 6);
    EXPECT_DOUBLE_EQ(*s.turnStartAversionPct, 100.0);
    EXPECT_FALSE(s.turnEndContactPct.has_value());
}

TEST(EyeContactResult, WithoutConversationTheSplitsAreAbsent) {
    QTemporaryDir dir;
    const auto r = EyeContactResult::load(
        write_file(dir.path(),
                   R"({"schema": "mosaic-eye-contact-v1", "target": {"method": "camera"},
            "summary": {"eye_contact_pct": 50.0, "aversions": 0}, "frames": []})"));
    ASSERT_TRUE(r.is_valid());
    EXPECT_FALSE(r.summary().listeningPct.has_value());
    EXPECT_FALSE(r.target_yaw().has_value());
    EXPECT_EQ(r.summary().subjectTurns, 0);
}

TEST(EyeContactResult, MissingOrForeignFileIsInvalid) {
    EXPECT_FALSE(EyeContactResult::load("/no/such/file.json").is_valid());
    QTemporaryDir dir;
    EXPECT_FALSE(EyeContactResult::load(
                     write_file(dir.path(), R"({"schema": "mosaic-gaze2d-v1", "frames": []})"))
                     .is_valid());
}
