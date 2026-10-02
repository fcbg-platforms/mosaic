#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QTextStream>

#include "analysis/sync_manifest.hpp"
#include "core/settings.hpp"
#include "video/interview_mode.hpp"

using namespace mosaic;

namespace {

VideoSettings six_cameras() {
    VideoSettings v;
    v.cameras.reserve(VideoSettings::kMaxCameras);
    for (int i = 0; i < 6; ++i) {
        CameraParameters c;
        c.serialNumber = QString("2489%1").arg(i);
        v.cameras.push_back(c);
    }
    return v;
}

void write_json(const QString& path, const QJsonObject& o) {
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QJsonDocument(o).toJson());
}

void write_cam2_timestamps(const QString& sessionDir, int frames, int64_t stepNs) {
    ASSERT_TRUE(QDir().mkpath(sessionDir + "/video"));
    QFile f(sessionDir + "/video/timestamps_cam2.csv");
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream ts(&f);
    ts << "frame_id,elapsed_ns,wall_ns\n";
    for (int i = 0; i < frames; ++i) {
        const int64_t e = static_cast<int64_t>(i) * stepNs;
        ts << i << ',' << e << ',' << (1'700'000'000'000'000'000LL + e) << '\n';
    }
}

} // namespace

// ── The merged parameters the interview camera opens with ──────────────────

TEST(InterviewMode, OverlaysCropRateAndExposureLimitOnTheCamerasOwnSettings) {
    CameraParameters room;
    room.serialNumber           = "24925620";
    room.balanceWhiteAuto       = "Off";
    room.balanceRatioRed        = 1.3;
    room.exposureAutoLowerUs    = 150.0;
    room.calibration.calibrated = true;

    InterviewSettings iv;
    iv.width               = 960;
    iv.height              = 720;
    iv.offsetX             = 480;
    iv.offsetY             = 180;
    iv.fps                 = 45.0;
    iv.exposureAutoUpperUs = 18000.0;

    const CameraParameters p = interview_camera_params(room, iv);

    EXPECT_EQ(p.width, 960);
    EXPECT_EQ(p.height, 720);
    EXPECT_EQ(p.offsetX, 480);
    EXPECT_EQ(p.offsetY, 180);
    EXPECT_DOUBLE_EQ(p.fps, 45.0);
    EXPECT_TRUE(p.specifyFps);
    EXPECT_DOUBLE_EQ(p.exposureAutoUpperUs, 18000.0);
    // Everything else is the camera's own: it is still tuned on its card.
    EXPECT_EQ(p.serialNumber, "24925620");
    EXPECT_EQ(p.balanceWhiteAuto, "Off");
    EXPECT_DOUBLE_EQ(p.balanceRatioRed, 1.3);
    EXPECT_DOUBLE_EQ(p.exposureAutoLowerUs, 150.0);
    EXPECT_TRUE(p.calibration.calibrated);
}

TEST(InterviewMode, OneCameraFreeRunsRatherThanWaitingForActionCommands) {
    CameraParameters room; // hwTriggerEnabled defaults to true (Action1)
    ASSERT_TRUE(room.hwTriggerEnabled);
    EXPECT_FALSE(interview_camera_params(room, InterviewSettings{}).hwTriggerEnabled);
}

TEST(InterviewMode, LowerExposureLimitNeverExceedsTheInterviewUpperLimit) {
    CameraParameters room;
    room.exposureAutoLowerUs = 30000.0;
    InterviewSettings iv;
    iv.exposureAutoUpperUs = 20000.0;
    EXPECT_DOUBLE_EQ(interview_camera_params(room, iv).exposureAutoLowerUs, 20000.0);
}

TEST(InterviewMode, TheRoomConfigurationIsLeftUntouched) {
    VideoSettings v               = six_cameras();
    v.interview.enabled           = true;
    const CameraParameters before = v.cameras[2];
    (void)interview_camera_params(v.cameras[2], v.interview);
    EXPECT_EQ(v.cameras[2].width, before.width);
    EXPECT_DOUBLE_EQ(v.cameras[2].fps, before.fps);
    EXPECT_EQ(v.cameras[2].hwTriggerEnabled, before.hwTriggerEnabled);
}

// ── Which cameras a session records ────────────────────────────────────────

TEST(InterviewMode, RoomModeRecordsEveryConfiguredCamera) {
    const VideoSettings v = six_cameras();
    EXPECT_FALSE(v.interview_active());
    EXPECT_EQ(v.recorded_camera_indices(), (std::vector<int>{0, 1, 2, 3, 4, 5}));
}

TEST(InterviewMode, InterviewModeRecordsOnlyItsCamera) {
    VideoSettings v     = six_cameras();
    v.interview.enabled = true;
    EXPECT_TRUE(v.interview_active());
    EXPECT_EQ(v.recorded_camera_indices(), (std::vector<int>{2}));
}

// Settings carried to a rig with fewer cameras must not record "only camera
// 3" on a rig that has no camera 3 — that would record nothing at all.
TEST(InterviewMode, AnIndexPastTheConfiguredCamerasMeansRoomMode) {
    VideoSettings v = six_cameras();
    v.cameras.resize(2);
    v.interview.enabled = true;
    EXPECT_FALSE(v.interview_active());
    EXPECT_EQ(v.recorded_camera_indices(), (std::vector<int>{0, 1}));

    v.interview.cameraIndex = -1;
    EXPECT_FALSE(v.interview_active());
}

// ── Persistence ────────────────────────────────────────────────────────────

TEST(InterviewMode, RoundTripsThroughVideoSettingsJson) {
    VideoSettings v                 = six_cameras();
    v.interview.enabled             = true;
    v.interview.cameraIndex         = 4;
    v.interview.width               = 640;
    v.interview.height              = 480;
    v.interview.offsetX             = 640;
    v.interview.offsetY             = 300;
    v.interview.fps                 = 55.5;
    v.interview.exposureAutoUpperUs = 15000.0;

    const auto back = VideoSettings::from_json(v.to_json());
    ASSERT_TRUE(back.has_value());
    EXPECT_TRUE(back->interview.enabled);
    EXPECT_EQ(back->interview.cameraIndex, 4);
    EXPECT_EQ(back->interview.width, 640);
    EXPECT_EQ(back->interview.height, 480);
    EXPECT_EQ(back->interview.offsetX, 640);
    EXPECT_EQ(back->interview.offsetY, 300);
    EXPECT_DOUBLE_EQ(back->interview.fps, 55.5);
    EXPECT_DOUBLE_EQ(back->interview.exposureAutoUpperUs, 15000.0);
}

TEST(InterviewMode, AnOlderSettingsFileOpensInRoomModeWithDefaults) {
    QJsonObject o = six_cameras().to_json();
    o.remove("interview");
    const auto back = VideoSettings::from_json(o);
    ASSERT_TRUE(back.has_value());
    EXPECT_FALSE(back->interview.enabled);
    EXPECT_EQ(back->interview.cameraIndex, InterviewSettings{}.cameraIndex);
    EXPECT_EQ(back->cameras.size(), 6u);
}

TEST(InterviewMode, NonsenseValuesFallBackFieldByField) {
    const InterviewSettings s = InterviewSettings::from_json(QJsonObject{
        {"enabled", true},
        {"width", 0},
        {"height", -5},
        {"offset_x", -1},
        {"offset_y", 12},
        {"fps", 0.0},
        {"exposure_auto_upper_us", -3.0},
    });
    const InterviewSettings defaults;
    EXPECT_TRUE(s.enabled);
    EXPECT_EQ(s.width, defaults.width);
    EXPECT_EQ(s.height, defaults.height);
    EXPECT_EQ(s.offsetX, 0);
    EXPECT_EQ(s.offsetY, 12); // a good value next to bad ones is kept
    EXPECT_DOUBLE_EQ(s.fps, defaults.fps);
    EXPECT_DOUBLE_EQ(s.exposureAutoUpperUs, defaults.exposureAutoUpperUs);
}

// The shipped defaults must be self-consistent: an exposure limit that cannot
// reach the default rate would ship a mode that misses its own target.
TEST(InterviewMode, DefaultExposureLimitAllowsTheDefaultRate) {
    const InterviewSettings s;
    EXPECT_LE(s.exposureAutoUpperUs, max_exposure_us_for_fps(s.fps));
    EXPECT_TRUE(crop_fits(CameraParameters{}.width, CameraParameters{}.height, s.width, s.height,
                          s.offsetX, s.offsetY));
    EXPECT_EQ(s.offsetX, centred_offset(CameraParameters{}.width, s.width));
    EXPECT_EQ(s.offsetY, centred_offset(CameraParameters{}.height, s.height));
}

// ── Panel arithmetic ───────────────────────────────────────────────────────

TEST(InterviewModeMath, CentredOffsetIsHalfTheMarginRoundedDownToFour) {
    EXPECT_EQ(centred_offset(1920, 1280), 320);
    EXPECT_EQ(centred_offset(1080, 720), 180);
    EXPECT_EQ(centred_offset(1920, 1914), 0); // half-margin 3 -> 0
    EXPECT_EQ(centred_offset(1080, 1066), 4); // half-margin 7 -> 4
    EXPECT_EQ(centred_offset(1920, 1920), 0);
    EXPECT_EQ(centred_offset(640, 1280), 0); // crop larger than frame
}

TEST(InterviewModeMath, StreamRateIsPixelsTimesBytesTimesRate) {
    // 1280×720 at 2 B/px and 40 fps = 73.728 MB/s.
    EXPECT_NEAR(stream_mb_per_s(1280, 720, 40.0), 73.728, 1e-9);
    // Room 11's full frame at 25 fps — close to the link, which is why a
    // higher rate needs a crop.
    EXPECT_NEAR(stream_mb_per_s(1920, 1080, 25.0), 103.68, 1e-9);
    EXPECT_DOUBLE_EQ(stream_mb_per_s(0, 720, 40.0), 0.0);
    EXPECT_DOUBLE_EQ(stream_mb_per_s(1280, 720, 0.0), 0.0);
}

TEST(InterviewModeMath, MaxExposureForARate) {
    EXPECT_DOUBLE_EQ(max_exposure_us_for_fps(50.0), 20000.0);
    EXPECT_DOUBLE_EQ(max_exposure_us_for_fps(25.0), 40000.0);
    EXPECT_DOUBLE_EQ(max_exposure_us_for_fps(0.0), -1.0);
}

TEST(InterviewModeMath, CropFits) {
    EXPECT_TRUE(crop_fits(1920, 1080, 1280, 720, 320, 180));
    EXPECT_TRUE(crop_fits(1920, 1080, 1920, 1080, 0, 0));
    EXPECT_FALSE(crop_fits(1920, 1080, 1280, 720, 700, 180)); // runs off the right
    EXPECT_FALSE(crop_fits(1920, 1080, 1280, 720, 320, 400)); // runs off the bottom
    EXPECT_FALSE(crop_fits(1920, 1080, 1280, 720, -4, 0));
    EXPECT_FALSE(crop_fits(1920, 1080, 0, 720, 0, 0));
}

// ── The master timeline runs at the session's own rate ────────────────────

TEST(InterviewModeSync, RoomSessionsKeepTheTwentyFiveFpsTimeline) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // No metadata at all.
    EXPECT_DOUBLE_EQ(SyncManifest::default_master_fps(dir.path()), 25.0);
    // An older session, before "mode" existed.
    write_json(dir.path() + "/session_meta.json",
               QJsonObject{{"recording", QJsonObject{{"video_enabled", true}}}});
    EXPECT_DOUBLE_EQ(SyncManifest::default_master_fps(dir.path()), 25.0);
    // An explicit room session.
    write_json(dir.path() + "/session_meta.json",
               QJsonObject{{"recording", QJsonObject{{"mode", "room"}}}});
    EXPECT_DOUBLE_EQ(SyncManifest::default_master_fps(dir.path()), 25.0);
}

TEST(InterviewModeSync, InterviewSessionsUseTheirRecordedRate) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    write_json(dir.path() + "/session_meta.json",
               QJsonObject{{"recording", QJsonObject{{"mode", "interview"},
                                                     {"interview_camera", 2},
                                                     {"interview_fps", 40.0}}}});
    EXPECT_DOUBLE_EQ(SyncManifest::default_master_fps(dir.path()), 40.0);
}

TEST(InterviewModeSync, AnUnusableInterviewRateFallsBack) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (const double bad : {0.0, -10.0, 5000.0}) {
        write_json(
            dir.path() + "/session_meta.json",
            QJsonObject{{"recording", QJsonObject{{"mode", "interview"}, {"interview_fps", bad}}}});
        EXPECT_DOUBLE_EQ(SyncManifest::default_master_fps(dir.path()), 25.0) << bad;
    }
}

// The point of the above: generate() with no explicit rate must not decimate
// a 40 fps interview to a 25 fps timeline.
TEST(InterviewModeSync, GenerateWithoutARateUsesTheInterviewRate) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    write_json(
        dir.path() + "/session_meta.json",
        QJsonObject{{"recording", QJsonObject{{"mode", "interview"}, {"interview_fps", 40.0}}}});
    write_cam2_timestamps(dir.path(), 80, 25'000'000LL); // 2 s at 40 fps

    const SyncManifest m = SyncManifest::generate(dir.path());
    ASSERT_TRUE(m.is_valid());
    EXPECT_DOUBLE_EQ(m.master_fps(), 40.0);
    ASSERT_EQ(m.camera_count(), 1);
    EXPECT_EQ(m.camera_info(0).index, 2);
    // Every recorded frame has its own tick — none skipped.
    QSet<int> seen;
    for (int t = 0; t < m.total_ticks(); ++t) {
        const int f = m.frame_at_tick(0, t);
        if (f >= 0) seen.insert(f);
    }
    EXPECT_GE(seen.size(), 79);

    // And an explicit rate still wins.
    EXPECT_DOUBLE_EQ(SyncManifest::generate(dir.path(), 25.0).master_fps(), 25.0);
}

// One camera has nothing to stagger its transmission against, and on this rig
// the stagger delay (index × 5 ms, 10 ms for Camera 3) appeared as a fixed
// 10 ms in every frame's time — capping 1280×720 at 36.7 fps.
TEST(InterviewMode, TheInterviewCameraDoesNotStaggerItsTransmission) {
    CameraParameters room;
    ASSERT_TRUE(room.staggerTransmission); // the room keeps its tested setting
    EXPECT_FALSE(interview_camera_params(room, InterviewSettings{}).staggerTransmission);
}
