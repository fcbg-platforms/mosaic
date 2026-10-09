#include <gtest/gtest.h>

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <cstdint>
#include <cstring>

#include "audio/wav_writer.hpp"
#include "session/session_end.hpp"
#include "video/frame_timestamp_writer.hpp"

using namespace mosaic;

// What a crash leaves behind is whatever had reached the file while the
// writer was still open. These tests read the files *before* close()/stop(),
// which is exactly the state a killed process leaves on disk.

namespace {

QByteArray read_all(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

uint32_t le32_at(const QByteArray& b, int offset) {
    uint32_t v = 0;
    std::memcpy(&v, b.constData() + offset, 4);
    return v; // the test machines are little-endian, like WAV
}

void write_json(const QString& path, const QJsonObject& o) {
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QJsonDocument(o).toJson());
}

QJsonObject read_json(const QString& path) {
    return QJsonDocument::fromJson(read_all(path)).object();
}

} // namespace

// ── WAV ────────────────────────────────────────────────────────────────────

TEST(CrashSafety, WavHeaderDeclaresTheSamplesWrittenWhileStillOpen) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("a.wav");

    WavWriter w;
    w.set_header_update_interval_ms(0);
    ASSERT_TRUE(w.open(path, 44100, 2, 16));
    const QByteArray chunk(4000, '\x11');
    ASSERT_TRUE(w.write(chunk.constData(), chunk.size()));
    ASSERT_TRUE(w.write(chunk.constData(), chunk.size()));

    const QByteArray onDisk = read_all(path);
    ASSERT_GE(onDisk.size(), 44 + 8000);
    EXPECT_EQ(le32_at(onDisk, 40), 8000u);      // data chunk size
    EXPECT_EQ(le32_at(onDisk, 4), 36u + 8000u); // RIFF size

    w.close();
    const QByteArray closed = read_all(path);
    EXPECT_EQ(le32_at(closed, 40), 8000u);
    EXPECT_EQ(closed.size(), 44 + 8000);
}

TEST(CrashSafety, WavHeaderIsNotRewrittenOnEveryWriteByDefault) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("b.wav");

    WavWriter w; // default 1 s interval
    ASSERT_TRUE(w.open(path, 44100, 2, 16));
    const QByteArray chunk(4000, '\x22');
    ASSERT_TRUE(w.write(chunk.constData(), chunk.size()));
    // Within the interval nothing is promised — the header may still be in
    // QFile's buffer. Once it has passed, the next write brings it up to date.
    QThread::msleep(1100);
    ASSERT_TRUE(w.write(chunk.constData(), chunk.size()));
    EXPECT_EQ(le32_at(read_all(path), 40), 8000u);
    w.close();
}

// ── Frame timestamps ───────────────────────────────────────────────────────

TEST(CrashSafety, TimestampRowsReachTheFileWhileStillOpen) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("timestamps_cam0.csv");

    FrameTimestampWriter w;
    w.set_flush_interval_ms(0);
    ASSERT_TRUE(w.start(path));
    for (int i = 1; i <= 50; ++i) {
        w.write(i, i * 40'000'000LL, 1'700'000'000'000'000'000LL + i, 0);
    }

    // Opened in text mode, so Windows line endings: normalise before splitting.
    const QList<QByteArray> lines = read_all(path).replace("\r\n", "\n").split('\n');
    // header + 50 rows + the empty string after the final newline
    ASSERT_EQ(lines.size(), 52);
    EXPECT_EQ(lines.front(), "frame_id,elapsed_ns,wall_ns,hw_timestamp_ns,exposure_us");
    EXPECT_TRUE(lines[50].startsWith("50,"));
    w.stop();
}

TEST(CrashSafety, TimestampRowsAreFlushedOnceTheIntervalPasses) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("timestamps_cam1.csv");

    FrameTimestampWriter w; // default 1 s
    ASSERT_TRUE(w.start(path));
    w.write(1, 1, 1, 0);
    QThread::msleep(1100);
    w.write(2, 2, 2, 0); // this write crosses the interval and flushes both
    const QByteArray onDisk = read_all(path).replace("\r\n", "\n");
    EXPECT_TRUE(onDisk.contains("\n1,1,1,0,\n"));
    EXPECT_TRUE(onDisk.contains("\n2,2,2,0,\n"));
    w.stop();
}

// The exposure column: the frame's own exposure in µs, fixed notation even
// for a long exposure, and empty (not 0 or -1) when the camera did not say.
TEST(FrameTimestamps, ExposureIsWrittenInMicrosecondsOrLeftEmpty) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("timestamps_cam2.csv");

    FrameTimestampWriter w;
    w.set_flush_interval_ms(0);
    ASSERT_TRUE(w.start(path));
    w.write(1, 10, 20, 30, 19998.75);
    w.write(2, 11, 21, 31, 1'000'000.0); // a 1 s exposure
    w.write(3, 12, 22, 32);              // unknown
    w.stop();

    const QList<QByteArray> lines = read_all(path).replace("\r\n", "\n").split('\n');
    ASSERT_GE(lines.size(), 4);
    EXPECT_EQ(lines[1], "1,10,20,30,19998.8");
    EXPECT_EQ(lines[2], "2,11,21,31,1000000.0");
    EXPECT_EQ(lines[3], "3,12,22,32,");
}

// ── Session end marker ─────────────────────────────────────────────────────

TEST(CrashSafety, SessionEndStateDistinguishesOldInterruptedAndClean) {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QJsonObject unfinished{{"session_end", QJsonValue::Null}};
    EXPECT_EQ(session_end_state(QJsonObject{{"schema", "mosaic-session-v1"}}, {}, now),
              SessionEnd::Unknown);
    EXPECT_EQ(session_end_state(unfinished, {}, now), SessionEnd::Interrupted);
    EXPECT_EQ(
        session_end_state(QJsonObject{{"session_end", QJsonObject{{"duration_ms", 5}}}}, {}, now),
        SessionEnd::Clean);
}

// A session being recorded has a null end too. Without the heartbeat, every
// refresh of the session list during a recording called it a crash.
TEST(CrashSafety, AnUnfinishedSessionWithARecentHeartbeatIsRecordingNotInterrupted) {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QJsonObject unfinished{{"session_end", QJsonValue::Null}};

    EXPECT_EQ(session_end_state(unfinished, now.addSecs(-3), now), SessionEnd::Recording);
    EXPECT_EQ(session_end_state(unfinished, now.addSecs(-(kHeartbeatStaleSec - 1)), now),
              SessionEnd::Recording);
    EXPECT_EQ(session_end_state(unfinished, now.addSecs(-kHeartbeatStaleSec), now),
              SessionEnd::Interrupted);
    EXPECT_EQ(session_end_state(unfinished, now.addSecs(-3600), now), SessionEnd::Interrupted);
    // Another machine's clock a little ahead of this one's.
    EXPECT_EQ(session_end_state(unfinished, now.addSecs(4), now), SessionEnd::Recording);
    // A finished session stays finished whatever its heartbeat says.
    EXPECT_EQ(session_end_state(QJsonObject{{"session_end", QJsonObject{}}}, now, now),
              SessionEnd::Clean);
    // The beat must outlast several missed intervals, not just one.
    EXPECT_GE(kHeartbeatStaleSec * 1000, 3 * kHeartbeatIntervalMs);
}

TEST(CrashSafety, HeartbeatRoundTripsAndIsRemovedOnACleanStop) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    EXPECT_FALSE(read_heartbeat(dir.path()).isValid());

    const QDateTime t = QDateTime::fromString("2026-10-01T10:00:05.250Z", Qt::ISODateWithMs);
    ASSERT_TRUE(write_heartbeat(dir.path(), t));
    EXPECT_EQ(read_heartbeat(dir.path()), t);

    remove_heartbeat(dir.path());
    EXPECT_FALSE(read_heartbeat(dir.path()).isValid());
}

TEST(CrashSafety, MarkingTheEndKeepsEveryOtherField) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QJsonObject start{
        {"schema", "mosaic-session-v1"},
        {"session_end", QJsonValue::Null},
        {"bids", QJsonObject{{"sub", "P01"}, {"run", 2}}},
        {"cameras", QJsonArray{QJsonObject{{"index", 2}, {"fps", 40.0}}}},
        {"recording", QJsonObject{{"mode", "interview"}}},
    };
    write_json(dir.filePath("session_meta.json"), start);

    const QDateTime end = QDateTime::fromString("2026-10-01T10:00:05.250Z", Qt::ISODateWithMs);
    ASSERT_TRUE(mark_session_ended(dir.path(), 65'000, end));

    const QJsonObject after = read_json(dir.filePath("session_meta.json"));
    EXPECT_EQ(session_end_state(after, {}, QDateTime::currentDateTimeUtc()), SessionEnd::Clean);
    const QJsonObject marker = after["session_end"].toObject();
    EXPECT_EQ(marker["duration_ms"].toInteger(), 65'000);
    EXPECT_TRUE(marker["ended_cleanly"].toBool());
    EXPECT_EQ(marker["utc"].toString(), "2026-10-01T10:00:05.250Z");
    for (const QString& key : {"schema", "bids", "cameras", "recording"}) {
        EXPECT_EQ(after[key], start[key]) << key.toStdString();
    }
}

TEST(CrashSafety, MarkingRefusesAnUnreadableMetaAndLeavesItAlone) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    EXPECT_FALSE(mark_session_ended(dir.path(), 1, QDateTime::currentDateTimeUtc())); // missing

    const QString path = dir.filePath("session_meta.json");
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("{ not json");
    }
    EXPECT_FALSE(mark_session_ended(dir.path(), 1, QDateTime::currentDateTimeUtc()));
    EXPECT_EQ(read_all(path), QByteArray("{ not json"));
}
