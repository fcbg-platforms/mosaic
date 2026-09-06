#include <gtest/gtest.h>

#include <QJsonObject>

#include "core/settings.hpp"

using mosaic::RecordSettings;

namespace {

// A record-settings object as written by a build predating
// RecordSettings::startDelaySec / hidePreviewsWhileRecording — i.e. with
// neither key present.
QJsonObject legacy_record_settings() {
    return QJsonObject{
        {"directory", "./recordings/guest"},
        {"video_basename", "video"},
        {"audio_basename", "audio"},
        {"trigger_basename", "trigger"},
        {"separator", "_"},
        {"add_timestamp", true},
        {"timestamp_format", "yyyy-MM-dd_hh-mm-ss"},
        {"enable_video", true},
        {"enable_audio", true},
        {"enable_trigger", true},
    };
}

} // namespace

// Upgrade path. A settings.json written before the start-countdown feature has
// neither key, and must load with the struct defaults rather than a zeroed
// delay or a hidden-previews flag the user never chose.
TEST(RecordPersistence, LegacySettingsWithoutStartKeysGetDefaults) {
    const auto loaded = RecordSettings::from_json(legacy_record_settings());
    ASSERT_TRUE(loaded.has_value());

    EXPECT_EQ(loaded->startDelaySec, 3);
    EXPECT_TRUE(loaded->hidePreviewsWhileRecording);

    // The rest of the entry must survive untouched.
    EXPECT_EQ(loaded->directory, "./recordings/guest");
    EXPECT_TRUE(loaded->enableVideo);
}

// The falsy values are the ones a default-as-fallback parse can silently
// swallow: QJsonValue::toInt(3) on a real 0, and toBool(true) on a real false.
// Both must survive, or a user who deliberately turned the countdown off would
// find it back at 3 on the next launch.
TEST(RecordPersistence, ExplicitZeroDelayAndFalseHideSurviveTheDefaultFallback) {
    QJsonObject o                      = legacy_record_settings();
    o["start_delay_sec"]               = 0;
    o["hide_previews_while_recording"] = false;

    const auto loaded = RecordSettings::from_json(o);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->startDelaySec, 0);
    EXPECT_FALSE(loaded->hidePreviewsWhileRecording);
}

TEST(RecordPersistence, StartKeysRoundTripThroughToJson) {
    RecordSettings s;
    s.startDelaySec              = 7;
    s.hidePreviewsWhileRecording = false;

    const auto parsed = RecordSettings::from_json(s.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->startDelaySec, 7);
    EXPECT_FALSE(parsed->hidePreviewsWhileRecording);
    // Unrelated fields still round-trip, so the two new keys didn't displace
    // anything in the flat brace-init list.
    EXPECT_EQ(parsed->videoBasename, s.videoBasename);
    EXPECT_EQ(parsed->addTimestamp, s.addTimestamp);
}

// ── Session identity ───────────────────────────────────────────────────────

// Same upgrade path as above: a settings.json predating the identity fields
// must load with an empty identity, which is what keeps such an install on the
// legacy timestamp-only folder naming until someone types something.
TEST(RecordPersistence, LegacySettingsWithoutIdentityLoadEmpty) {
    const auto loaded = RecordSettings::from_json(legacy_record_settings());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_FALSE(loaded->lastIdentity.has_entities());
    EXPECT_EQ(loaded->lastIdentity.run, 0);
}

TEST(RecordPersistence, IdentityRoundTripsThroughToJson) {
    RecordSettings s;
    s.lastIdentity.subject = "P01";
    s.lastIdentity.session = "pre";
    s.lastIdentity.task    = "rest";

    const auto parsed = RecordSettings::from_json(s.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->lastIdentity.subject, "P01");
    EXPECT_EQ(parsed->lastIdentity.session, "pre");
    EXPECT_EQ(parsed->lastIdentity.task, "rest");
}

// settings.json is a plain file a user can edit, and these values go on to
// form a directory name — so the load path, not just the UI, has to sanitize.
TEST(RecordPersistence, HandEditedIdentityIsSanitizedOnLoad) {
    QJsonObject o      = legacy_record_settings();
    o["last_identity"] = QJsonObject{{"sub", "../../etc"}, {"ses", "p re"}, {"task", "re-st"}};

    const auto loaded = RecordSettings::from_json(o);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->lastIdentity.subject, "etc");
    EXPECT_EQ(loaded->lastIdentity.session, "pre");
    EXPECT_EQ(loaded->lastIdentity.task, "rest");
}
