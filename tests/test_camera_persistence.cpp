#include "auth/profile_manager.hpp"
#include "core/settings.hpp"
#include <gtest/gtest.h>
#include <QFileInfo>
#include <QTemporaryDir>

using mosaic::AppSettings;
using mosaic::CameraParameters;
using mosaic::ProfileManager;

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
