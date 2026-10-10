#include <gtest/gtest.h>

#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>

#include "auth/profile_manager.hpp"
#include "core/settings.hpp"

using mosaic::AppSettings;
using mosaic::CameraParameters;
using mosaic::ProfileManager;
using mosaic::VideoSettings;

// Regression test for the room-11 "drop a camera, does it come back on
// relaunch" question: VideoSettings::cameras is a plain runtime vector with
// no fixed roster, so removing an entry and saving/reloading should persist
// exactly the remaining cameras — not silently restore the original count.
TEST(CameraPersistence, DroppedCameraStaysDroppedAfterSaveAndLoad) {
    AppSettings settings;
    CameraParameters cam0;
    cam0.serialNumber = "111";
    CameraParameters cam1;
    cam1.serialNumber = "222";
    CameraParameters cam2;
    cam2.serialNumber      = "333";
    settings.video.cameras = {cam0, cam1, cam2};

    // Exercises the same vector::erase() the persistence layer itself relies
    // on — the GUI no longer offers a way to drop a camera (removed so a
    // configured rig can't be accidentally shrunk), but a dropped-and-saved
    // entry should still stay dropped after a reload.
    settings.video.cameras.erase(settings.video.cameras.begin() + 1);
    ASSERT_EQ(settings.video.cameras.size(), 2u);

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    const QString path = tmpDir.filePath("settings.json");

    ASSERT_TRUE(settings.save(path));

    const auto loaded = AppSettings::load(path);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->video.cameras.size(), 2u);
    EXPECT_EQ(loaded->video.cameras[0].serialNumber, QString("111"));
    EXPECT_EQ(loaded->video.cameras[1].serialNumber, QString("333"));
}

// Regression test for a real use-after-free: VideoManager::open() binds a
// raw `const CameraParameters&` into each VideoGrabber for that grabber's
// entire lifetime (see VideoGrabber::Impl::params), pointing directly into
// VideoSettings::cameras's vector storage. VideoSettingsW's constructor
// later reserve()s VideoSettings::kMaxCameras against that same vector —
// if VideoSettings::from_json() hadn't already reserved at least that much
// capacity when it first populated `cameras`, that reserve() (or any
// push_back() past whatever smaller capacity from_json() left behind)
// would reallocate and silently invalidate every already-bound
// VideoGrabber reference, crashing the next time a camera control is
// edited. This can't be exercised end-to-end without real hardware, so
// this test asserts the one practically-testable invariant: from_json()
// leaves enough capacity headroom that no code path relying on it can
// trigger that reallocation.
TEST(CameraPersistence, FromJsonReservesCapacityForLiveReferenceStability) {
    QJsonArray cams;
    for (int i = 0; i < 3; ++i) {
        CameraParameters c;
        c.serialNumber = QString("cam-%1").arg(i);
        cams.append(c.to_json());
    }
    const QJsonObject videoObj{{"cameras", cams}};

    const auto loaded = VideoSettings::from_json(videoObj);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->cameras.size(), 3u);
    EXPECT_GE(loaded->cameras.capacity(), static_cast<size_t>(VideoSettings::kMaxCameras));
}

// Regression test for "per-user settings should not clobber each other":
// two different profile usernames must resolve to distinct settings.json
// paths under distinct per-profile directories.
TEST(ProfileIsolation, DifferentUsernamesGetDistinctSettingsPaths) {
    const QString pathA = ProfileManager::settings_path("lab_alpha");
    const QString pathB = ProfileManager::settings_path("lab_beta");

    EXPECT_NE(pathA, pathB);
    EXPECT_EQ(ProfileManager::profile_dir("lab_alpha"), QFileInfo(pathA).absolutePath());
    EXPECT_EQ(ProfileManager::profile_dir("lab_beta"), QFileInfo(pathB).absolutePath());
    EXPECT_NE(ProfileManager::profile_dir("lab_alpha"), ProfileManager::profile_dir("lab_beta"));
}

// ── Legacy auto-exposure limit migration ───────────────────────────────────

namespace {
mosaic::CameraParameters camera_with(double fps, double upperUs) {
    mosaic::CameraParameters c;
    c.fps                 = fps;
    c.exposureAutoUpperUs = upperUs;
    return c;
}
} // namespace

// Room 11: 25 fps cameras on the untouched 50 ms default, which caps them at
// 20 fps now that the limit actually reaches the camera.
TEST(LegacyExposureLimit, TheOldDefaultIsMovedWhereItBlocksTheConfiguredRate) {
    VideoSettings v;
    v.cameras = {camera_with(25.0, 50000.0), camera_with(25.0, 50000.0)};
    EXPECT_EQ(v.migrate_legacy_exposure_limit(), 2);
    EXPECT_DOUBLE_EQ(v.cameras[0].exposureAutoUpperUs, CameraParameters{}.exposureAutoUpperUs);
}

// The guest profile runs at 15 fps; 50 ms allows 20, so it does not constrain
// anything and shortening it would only darken the image.
TEST(LegacyExposureLimit, TheOldDefaultIsLeftWhereItDoesNotBlockAnything) {
    VideoSettings v;
    v.cameras = {camera_with(15.0, 50000.0)};
    EXPECT_EQ(v.migrate_legacy_exposure_limit(), 0);
    EXPECT_DOUBLE_EQ(v.cameras[0].exposureAutoUpperUs, 50000.0);
}

// Any value other than the exact old default is an operator's choice.
TEST(LegacyExposureLimit, AnOperatorsOwnValueIsNeverTouched) {
    VideoSettings v;
    v.cameras = {camera_with(25.0, 60000.0), camera_with(25.0, 49999.0)};
    EXPECT_EQ(v.migrate_legacy_exposure_limit(), 0);
    EXPECT_DOUBLE_EQ(v.cameras[0].exposureAutoUpperUs, 60000.0);
    EXPECT_DOUBLE_EQ(v.cameras[1].exposureAutoUpperUs, 49999.0);
}

// A free-running camera has no configured rate for the limit to block.
TEST(LegacyExposureLimit, AFreeRunningCameraIsLeftAlone) {
    VideoSettings v;
    v.cameras               = {camera_with(25.0, 50000.0)};
    v.cameras[0].specifyFps = false;
    EXPECT_EQ(v.migrate_legacy_exposure_limit(), 0);
}

// Loading must not migrate: load() is also used to read other profiles.
TEST(LegacyExposureLimit, LoadingAloneDoesNotMigrate) {
    AppSettings settings;
    settings.video.cameras = {camera_with(25.0, 50000.0)};
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("settings.json");
    ASSERT_TRUE(settings.save(path));
    const auto loaded = AppSettings::load(path);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_DOUBLE_EQ(loaded->video.cameras[0].exposureAutoUpperUs, 50000.0);
}

// Gaze target regions travel with the room settings (and so into every
// session's metadata); one without a name or a size is dropped on load
// rather than kept as an invisible target.
TEST(RoomPersistence, GazeRegionsRoundTripAndInvalidOnesAreDropped) {
    mosaic::RoomSettings room;
    mosaic::GazeRegion screen;
    screen.name   = "screen";
    screen.centre = {10.0, -200.0, 1800.0};
    screen.normal = {0.0, 0.0, -1.0};
    screen.uAxis  = {1.0, 0.0, 0.0};
    screen.width  = 600.0;
    screen.height = 340.0;
    room.regions.push_back(screen);
    mosaic::GazeRegion unnamed = screen;
    unnamed.name               = "";
    room.regions.push_back(unnamed);

    const auto loaded = mosaic::RoomSettings::from_json(room.to_json());
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->regions.size(), 1);
    EXPECT_EQ(loaded->regions[0].name, "screen");
    EXPECT_DOUBLE_EQ(loaded->regions[0].centre[1], -200.0);
    EXPECT_DOUBLE_EQ(loaded->regions[0].normal[2], -1.0);
    EXPECT_DOUBLE_EQ(loaded->regions[0].height, 340.0);
}

// The calibrated image's size and crop survive a save; settings from before
// they were recorded load as unknown (-1), never as a full-frame 0 offset.
TEST(CalibrationPersistence, ImageSizeAndCropRoundTripAndDefaultToUnknown) {
    mosaic::CalibrationData cal;
    cal.calibrated    = true;
    cal.imageWidth    = 1280;
    cal.imageHeight   = 540;
    cal.imageOffsetX  = 320;
    cal.imageOffsetY  = 270;
    const auto loaded = mosaic::CalibrationData::from_json(cal.to_json());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->imageWidth, 1280);
    EXPECT_EQ(loaded->imageOffsetY, 270);

    const auto legacy = mosaic::CalibrationData::from_json(QJsonObject{{"calibrated", true}});
    ASSERT_TRUE(legacy.has_value());
    EXPECT_EQ(legacy->imageWidth, -1);
    EXPECT_EQ(legacy->imageOffsetX, -1);
}
