#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <cmath>

#include "analysis/voice_result.hpp"

using mosaic::VoiceResult;

namespace {

// A complete, well-formed sidecar. Individual tests mutate one key at a time so
// each failure names exactly one cause.
QString full_json(const QString& overrides = {}) {
    return QString(R"({
      "schema_version": 1,
      "source_audio": "audio_0.wav",
      "duration_ms": 4000.0,
      "spectrogram": {
        "image": "audio_0.voice.png",
        "width": 512, "height": 256,
        "row_order": "high_to_low",
        "t0_ms": 0.0, "t1_ms": 4000.0,
        "f0_hz": 0.0, "f1_hz": 8000.0,
        "db_min": -20.0, "db_max": 50.0
      },
      "pitch": {
        "t0_ms": 0.0, "dt_ms": 10.0, "floor_hz": 60.0, "ceiling_hz": 600.0,
        "values_hz": [0.0, 120.5, 121.0, 0.0]
      },
      "intensity": { "t0_ms": 0.0, "dt_ms": 10.0, "values_db": [40.0, 55.0, 54.0, 30.0] }
      %1
    })")
        .arg(overrides);
}

QString write(QTemporaryDir& dir, const QString& body, const QString& name = "a.voice.json") {
    const QString path = dir.filePath(name);
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(body.toUtf8());
    f.close();
    return path;
}

// Replaces a "key": value pair in the fixture. Crude but keeps each test's
// intent to a single visible line.
QString with(const QString& json, const QString& from, const QString& to) {
    QString out = json;
    EXPECT_TRUE(out.contains(from)) << from.toStdString();
    return out.replace(from, to);
}

} // namespace

TEST(VoiceResult, ParsesAWellFormedFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto r = VoiceResult::load(write(dir, full_json()));

    ASSERT_TRUE(r.is_valid());
    EXPECT_EQ(r.source_audio(), "audio_0.wav");
    EXPECT_DOUBLE_EQ(r.duration_ms(), 4000.0);
    EXPECT_EQ(r.spectrogram().width, 512);
    EXPECT_EQ(r.spectrogram().height, 256);
    EXPECT_DOUBLE_EQ(r.spectrogram().f1Hz, 8000.0);
    EXPECT_TRUE(r.spectrogram().has_image());
    EXPECT_EQ(r.pitch().values.size(), 4);
    EXPECT_DOUBLE_EQ(r.pitch_ceiling_hz(), 600.0);
    EXPECT_EQ(r.intensity().values.size(), 4);
}

TEST(VoiceResult, MissingOrMalformedFileIsInvalidNotCrashing) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    EXPECT_FALSE(VoiceResult::load(dir.filePath("nope.json")).is_valid());
    EXPECT_FALSE(VoiceResult::load(write(dir, "{ not json at all")).is_valid());
    EXPECT_FALSE(VoiceResult::load(write(dir, "{}")).is_valid());
}

// A file from a future build describes pixels this one doesn't know how to
// interpret. Refusing beats rendering something confidently wrong.
TEST(VoiceResult, ForeignSchemaVersionIsRejected) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    EXPECT_FALSE(VoiceResult::load(write(dir, with(full_json(), R"("schema_version": 1)",
                                                   R"("schema_version": 2)")))
                     .is_valid());
    // Absent is not "assume 1".
    EXPECT_FALSE(
        VoiceResult::load(write(dir, with(full_json(), R"("schema_version": 1,)", ""))).is_valid());
}

// THE upside-down guard. A mirrored spectrogram still has formants, still has
// silence in the right places, and still tracks the audio — nobody notices for
// a week. So an unrecognised row order is refused rather than guessed.
TEST(VoiceResult, UnknownRowOrderIsRejected) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (const char* order : {R"("row_order": "low_to_high")", R"("row_order": "")"}) {
        EXPECT_FALSE(
            VoiceResult::load(write(dir, with(full_json(), R"("row_order": "high_to_low")", order)))
                .is_valid())
            << order;
    }
}

TEST(VoiceResult, ImagePathResolvesBesideTheJson) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto r = VoiceResult::load(write(dir, full_json(), "sub.voice.json"));
    ASSERT_TRUE(r.is_valid());
    EXPECT_EQ(QFileInfo(r.spectrogram().imagePath).path(), QFileInfo(dir.filePath("x")).path());
    EXPECT_EQ(QFileInfo(r.spectrogram().imagePath).fileName(), "audio_0.voice.png");
}

// The image name is untrusted text that becomes a path. It must not be able to
// point outside the session — but a bad reference must not throw away the
// tracks either, which are perfectly good data.
TEST(VoiceResult, EscapingOrAbsoluteImagePathsAreDroppedButTheRestSurvives) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (const char* bad : {R"("image": "../../secrets.png")", R"("image": "C:/windows/x.png")",
                            R"("image": "/etc/passwd")", R"("image": "sub/dir/x.png")"}) {
        const auto r = VoiceResult::load(
            write(dir, with(full_json(), R"("image": "audio_0.voice.png")", bad)));
        ASSERT_TRUE(r.is_valid()) << bad;
        EXPECT_FALSE(r.spectrogram().has_image()) << bad;
        EXPECT_EQ(r.pitch().values.size(), 4) << bad; // tracks kept
    }
}

TEST(VoiceResult, TrackLookupFindsTheNearestFrame) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto r = VoiceResult::load(write(dir, full_json()));
    ASSERT_TRUE(r.is_valid());

    EXPECT_FLOAT_EQ(r.pitch().at(10.0, -1.0f), 120.5f);
    EXPECT_FLOAT_EQ(r.pitch().at(13.0, -1.0f), 120.5f); // rounds to frame 1
    EXPECT_FLOAT_EQ(r.pitch().at(17.0, -1.0f), 121.0f); // rounds to frame 2
}

// 0 Hz means "unvoiced", a value rather than an absence — the widget must break
// its line there instead of interpolating a note nobody sang.
TEST(VoiceResult, UnvoicedFramesReadAsZeroNotAsMissing) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto r = VoiceResult::load(write(dir, full_json()));
    ASSERT_TRUE(r.is_valid());
    EXPECT_FLOAT_EQ(r.pitch().at(0.0, -1.0f), 0.0f);
    EXPECT_FLOAT_EQ(r.pitch().at(30.0, -1.0f), 0.0f);
}

TEST(VoiceResult, LookupsOutsideTheTrackReturnTheFallback) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto r = VoiceResult::load(write(dir, full_json()));
    ASSERT_TRUE(r.is_valid());
    EXPECT_FLOAT_EQ(r.pitch().at(-500.0, -1.0f), -1.0f);
    EXPECT_FLOAT_EQ(r.pitch().at(99'999.0, -1.0f), -1.0f);
    EXPECT_TRUE(std::isnan(r.intensity().at(99'999.0, std::nanf(""))));
}

// Every lookup divides by dtMs, so a zero must make the track invalid rather
// than reach the arithmetic.
TEST(VoiceResult, ZeroTimeStepMakesATrackInvalid) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto r =
        VoiceResult::load(write(dir, with(full_json(), R"("t0_ms": 0.0, "dt_ms": 10.0, "floor_hz")",
                                          R"("t0_ms": 0.0, "dt_ms": 0.0, "floor_hz")")));
    ASSERT_TRUE(r.is_valid());
    EXPECT_FALSE(r.pitch().is_valid());
    EXPECT_FLOAT_EQ(r.pitch().at(10.0, -1.0f), -1.0f);
}

// A clip too short to analyse legitimately yields nothing. That must load as an
// empty panel, not as a failure.
TEST(VoiceResult, EmptyTracksAndNoImageStillLoad) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto r = VoiceResult::load(write(dir, R"({
        "schema_version": 1, "source_audio": "a.wav", "duration_ms": 120.0
    })"));
    ASSERT_TRUE(r.is_valid());
    EXPECT_FALSE(r.spectrogram().has_image());
    EXPECT_FALSE(r.pitch().is_valid());
    EXPECT_FALSE(r.intensity().is_valid());
}
