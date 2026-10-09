#include <gtest/gtest.h>

#include <QTemporaryDir>

#include "session/preflight.hpp"

using namespace mosaic;

namespace {

PreflightCamera healthy_camera(int configIndex) {
    PreflightCamera c;
    c.configIndex     = configIndex;
    c.opened          = true;
    c.grabberRunning  = true;
    c.lastFrameAgeSec = 0.04;
    c.wantsAction1    = true;
    c.action1Ready    = true;
    c.configuredFps   = 25.0;
    c.achievableFps   = 24.9;
    return c;
}

// Six healthy cameras, one present microphone, plenty of disk.
PreflightInput healthy_rig() {
    PreflightInput in;
    for (int i = 0; i < 6; ++i) in.cameras.push_back(healthy_camera(i));
    in.configuredMics = 1;
    in.bytesPerSec    = 4e6;                    // 4 MB/s
    in.freeBytes      = qint64(4e6 * 3600 * 5); // 5 h
    in.directory      = "D:/recordings";
    return in;
}

int count_level(const PreflightReport& r, PreflightLevel level) {
    int n = 0;
    for (const auto& item : r.items) n += item.level == level ? 1 : 0;
    return n;
}

bool has_title(const PreflightReport& r, const QString& title) {
    for (const auto& item : r.items) {
        if (item.title == title) return true;
    }
    return false;
}

qint64 bytes_for_minutes(double minutes, double bytesPerSec) {
    return static_cast<qint64>(minutes * 60.0 * bytesPerSec);
}

} // namespace

// The common case must not show a dialog: everything passes, nothing to say.
TEST(Preflight, AHealthyRigNeedsNoAttention) {
    const PreflightReport r = evaluate_preflight(healthy_rig());
    EXPECT_FALSE(r.needs_attention());
    EXPECT_EQ(r.worst(), PreflightLevel::Ok);
    EXPECT_TRUE(r.problems().isEmpty());
    // The passes are still reported, collapsed: one camera row, not six.
    EXPECT_TRUE(has_title(r, "6 cameras ready"));
    EXPECT_TRUE(has_title(r, "1 microphone ready"));
    EXPECT_TRUE(has_title(r, "Disk"));
}

// ── Cameras ────────────────────────────────────────────────────────────────

TEST(Preflight, ACameraThatDidNotOpenFailsAndIsNamedOneBased) {
    PreflightInput in       = healthy_rig();
    in.cameras[5].opened    = false; // configured index 5 = "Camera 6"
    const PreflightReport r = evaluate_preflight(in);
    EXPECT_EQ(r.worst(), PreflightLevel::Fail);
    ASSERT_FALSE(r.items.isEmpty());
    EXPECT_EQ(r.items.front().title, "Camera 6 is not open"); // fails sort first
    EXPECT_TRUE(has_title(r, "5 cameras ready"));
}

TEST(Preflight, AnOpenCameraWithNoFramesFails) {
    PreflightInput in             = healthy_rig();
    in.cameras[1].lastFrameAgeSec = -1.0; // none ever arrived
    const PreflightReport r       = evaluate_preflight(in);
    EXPECT_TRUE(has_title(r, "Camera 2 is not delivering frames"));
    EXPECT_EQ(r.worst(), PreflightLevel::Fail);

    in                           = healthy_rig();
    in.cameras[1].grabberRunning = false;
    EXPECT_TRUE(has_title(evaluate_preflight(in), "Camera 2 is not delivering frames"));
}

// The case the first version missed: frames arrived once, then the link
// dropped. The grabber keeps running and the last timestamp stays put; only
// its age shows the stream has stopped.
TEST(Preflight, ACameraWhoseFramesStoppedFails) {
    PreflightInput in             = healthy_rig();
    in.cameras[4].lastFrameAgeSec = 600.0;
    const PreflightReport r       = evaluate_preflight(in);
    EXPECT_EQ(r.worst(), PreflightLevel::Fail);
    EXPECT_TRUE(has_title(r, "Camera 5 has stopped delivering frames"));
    EXPECT_TRUE(has_title(r, "5 cameras ready"));

    in.cameras[4].lastFrameAgeSec = kPreflightStaleFrameSec - 0.5; // slow, not stalled
    EXPECT_FALSE(evaluate_preflight(in).needs_attention());
}

// A camera being reconnected has no fresh frame either, but "stopped
// delivering ... its video would stop there" is no longer true: it resumes
// into the same video once it is back. Say what is actually happening.
TEST(Preflight, ACameraThatDroppedOutSaysItIsReconnecting) {
    PreflightInput in             = healthy_rig();
    in.cameras[2].reconnecting    = true;
    in.cameras[2].lastFrameAgeSec = -1.0; // preview restarted while it was away
    const PreflightReport r       = evaluate_preflight(in);
    EXPECT_EQ(r.worst(), PreflightLevel::Fail);
    EXPECT_TRUE(has_title(r, "Camera 3 has dropped out"));
    EXPECT_FALSE(has_title(r, "Camera 3 is not delivering frames"));
    EXPECT_TRUE(has_title(r, "5 cameras ready"));
}

// Without a pinned rate the camera runs as fast as it can, and the fps field
// is unused — it must not produce a warning on every click.
TEST(Preflight, ACameraWithoutAFixedRateIsNeverBelowIt) {
    PreflightInput in           = healthy_rig();
    in.cameras[0].fixedRate     = false;
    in.cameras[0].configuredFps = 30.0;
    in.cameras[0].achievableFps = 20.0;
    EXPECT_FALSE(evaluate_preflight(in).needs_attention());
}

TEST(Preflight, ACameraWellBelowItsRateWarns) {
    PreflightInput in           = healthy_rig();
    in.cameras[2].achievableFps = 14.45; // the auto-exposure ceiling this rig had for months
    const PreflightReport r     = evaluate_preflight(in);
    EXPECT_EQ(r.worst(), PreflightLevel::Warn);
    EXPECT_TRUE(has_title(r, "Camera 3 is below its frame rate"));
}

TEST(Preflight, AShortfallInsideTheToleranceIsNotAWarning) {
    PreflightInput in           = healthy_rig();
    in.cameras[0].achievableFps = 25.0 * 0.91; // above k_fps_shortfall_factor
    EXPECT_FALSE(evaluate_preflight(in).needs_attention());
}

// A camera that has not been grabbing long enough reports -1; that is "not
// known yet", not "slow", and must not be dressed up as a problem.
TEST(Preflight, AnUnmeasuredRateIsNotAProblem) {
    PreflightInput in = healthy_rig();
    for (auto& c : in.cameras) c.achievableFps = -1.0;
    const PreflightReport r = evaluate_preflight(in);
    EXPECT_FALSE(r.needs_attention());
    ASSERT_TRUE(has_title(r, "6 cameras ready"));
    for (const auto& item : r.items) {
        if (item.title == "6 cameras ready") {
            EXPECT_TRUE(item.detail.contains("still measuring")) << item.detail.toStdString();
            EXPECT_FALSE(item.detail.contains("at their frame rate"));
        }
    }
}

TEST(Preflight, Action1FallenBackToFreeRunWarns) {
    PreflightInput in          = healthy_rig();
    in.cameras[3].action1Ready = false;
    const PreflightReport r    = evaluate_preflight(in);
    EXPECT_EQ(r.worst(), PreflightLevel::Warn);
    EXPECT_TRUE(has_title(r, "Camera 4 is not synchronised"));
}

TEST(Preflight, FreeRunningByChoiceIsNotASyncProblem) {
    PreflightInput in = healthy_rig();
    for (auto& c : in.cameras) {
        c.wantsAction1 = false;
        c.action1Ready = false;
    }
    EXPECT_FALSE(evaluate_preflight(in).needs_attention());
}

TEST(Preflight, VideoOffMeansNoCameraRows) {
    PreflightInput in       = healthy_rig();
    in.videoEnabled         = false;
    in.cameras[0].opened    = false; // would fail if cameras were checked
    const PreflightReport r = evaluate_preflight(in);
    EXPECT_FALSE(r.needs_attention());
    for (const auto& item : r.items) {
        EXPECT_FALSE(item.title.contains("amera")) << item.title.toStdString();
    }
}

TEST(Preflight, VideoOnWithNoCamerasFails) {
    PreflightInput in = healthy_rig();
    in.cameras.clear();
    const PreflightReport r = evaluate_preflight(in);
    EXPECT_EQ(r.worst(), PreflightLevel::Fail);
    EXPECT_TRUE(has_title(r, "No cameras configured"));
}

// ── Microphones ────────────────────────────────────────────────────────────

TEST(Preflight, AMissingMicrophoneWarns) {
    PreflightInput in       = healthy_rig();
    in.missingMics          = {"RODE NT-USB"};
    const PreflightReport r = evaluate_preflight(in);
    EXPECT_EQ(r.worst(), PreflightLevel::Warn);
    EXPECT_TRUE(has_title(r, "RODE NT-USB not found"));
}

TEST(Preflight, AudioOnWithNoMicrophonesWarns) {
    PreflightInput in = healthy_rig();
    in.configuredMics = 0;
    EXPECT_TRUE(has_title(evaluate_preflight(in), "No microphones configured"));
}

TEST(Preflight, AudioOffMeansNoMicrophoneRows) {
    PreflightInput in       = healthy_rig();
    in.audioEnabled         = false;
    in.missingMics          = {"RODE NT-USB"};
    const PreflightReport r = evaluate_preflight(in);
    EXPECT_FALSE(r.needs_attention());
    EXPECT_FALSE(has_title(r, "1 microphone ready"));
}

// ── Disk ───────────────────────────────────────────────────────────────────

TEST(Preflight, DiskThresholds) {
    PreflightInput in = healthy_rig();
    const double rate = in.bytesPerSec;

    in.freeBytes = bytes_for_minutes(9, rate);
    EXPECT_TRUE(has_title(evaluate_preflight(in), "Disk almost full"));
    EXPECT_EQ(evaluate_preflight(in).worst(), PreflightLevel::Fail);

    in.freeBytes = bytes_for_minutes(11, rate);
    EXPECT_TRUE(has_title(evaluate_preflight(in), "Disk space low"));
    EXPECT_EQ(evaluate_preflight(in).worst(), PreflightLevel::Warn);

    in.freeBytes = bytes_for_minutes(59, rate);
    EXPECT_EQ(evaluate_preflight(in).worst(), PreflightLevel::Warn);

    in.freeBytes = bytes_for_minutes(61, rate);
    EXPECT_FALSE(evaluate_preflight(in).needs_attention());
}

TEST(Preflight, UnknownFreeSpaceWarns) {
    PreflightInput in = healthy_rig();
    in.freeBytes      = -1;
    EXPECT_TRUE(has_title(evaluate_preflight(in), "Disk space unknown"));
}

TEST(Preflight, NothingToWriteIsNotADivisionByZero) {
    PreflightInput in       = healthy_rig();
    in.bytesPerSec          = 0.0;
    in.freeBytes            = 1000;
    const PreflightReport r = evaluate_preflight(in);
    EXPECT_FALSE(r.needs_attention());
    EXPECT_TRUE(has_title(r, "Disk"));
}

TEST(Preflight, TheDiskRowNamesTheFolderAndTheTime) {
    const PreflightReport r = evaluate_preflight(healthy_rig());
    for (const auto& item : r.items) {
        if (item.title == "Disk") {
            EXPECT_TRUE(item.detail.contains("D:/recordings")) << item.detail.toStdString();
            EXPECT_TRUE(item.detail.contains("5 h")) << item.detail.toStdString();
        }
    }
}

// ── Report ─────────────────────────────────────────────────────────────────

TEST(Preflight, ProblemsListOnlyWarningsAndFailuresFailuresFirst) {
    PreflightInput in           = healthy_rig();
    in.cameras[2].achievableFps = 10.0;  // warn
    in.cameras[5].opened        = false; // fail
    const QStringList p         = evaluate_preflight(in).problems();
    ASSERT_EQ(p.size(), 2);
    EXPECT_TRUE(p[0].startsWith("Camera 6 is not open — "));
    EXPECT_TRUE(p[1].startsWith("Camera 3 is below its frame rate — "));
}

// ── Helpers ────────────────────────────────────────────────────────────────

TEST(Preflight, RecordingRateEstimate) {
    // 6 cameras at 5000 kbit/s = 3.75 MB/s; one 44.1 kHz stereo PCM mic = 176 400 B/s.
    EXPECT_DOUBLE_EQ(estimate_recording_bytes_per_sec(6, 5000, 1, 44100, 2, "pcm_s16le"),
                     3'750'000.0 + 176'400.0);
    // Compressed audio counted at a quarter.
    EXPECT_DOUBLE_EQ(estimate_recording_bytes_per_sec(0, 0, 1, 44100, 2, "flac"), 44'100.0);
    EXPECT_DOUBLE_EQ(estimate_recording_bytes_per_sec(0, 5000, 0, 44100, 2, "pcm_s16le"), 0.0);
}

// The recordings folder of a new user does not exist until their first
// recording; the volume it will land on must still be measurable.
TEST(Preflight, FreeSpaceOfAFolderThatDoesNotExistYet) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    EXPECT_GT(free_bytes_for(dir.filePath("recordings/newuser")), 0);
    EXPECT_GT(free_bytes_for(dir.path()), 0);
}
