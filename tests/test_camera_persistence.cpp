#include "auth/profile_manager.hpp"
#include "core/settings.hpp"
#include <gtest/gtest.h>
#include <QFileInfo>
#include <QTemporaryDir>

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
    CameraParameters cam0; cam0.serialNumber = "111";
    CameraParameters cam1; cam1.serialNumber = "222";
    CameraParameters cam2; cam2.serialNumber = "333";
    settings.video.cameras = {cam0, cam1, cam2};

    // Same erase() the "-" button on a camera card performs
    // (VideoSettingsW::remove_camera).
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
    EXPECT_GE(loaded->cameras.capacity(),
              static_cast<size_t>(VideoSettings::kMaxCameras));
}

// Regression test for "per-user settings should not clobber each other":
// two different profile usernames must resolve to distinct settings.json
// paths under distinct per-profile directories.
TEST(ProfileIsolation, DifferentUsernamesGetDistinctSettingsPaths) {
    const QString pathA = ProfileManager::settings_path("lab_alpha");
    const QString pathB = ProfileManager::settings_path("lab_beta");

    EXPECT_NE(pathA, pathB);
    EXPECT_EQ(ProfileManager::profile_dir("lab_alpha"), QFileInfo(pathA).absolutePath());
    EXPECT_EQ(ProfileManager::profile_dir("lab_beta"),  QFileInfo(pathB).absolutePath());
    EXPECT_NE(ProfileManager::profile_dir("lab_alpha"), ProfileManager::profile_dir("lab_beta"));
}
