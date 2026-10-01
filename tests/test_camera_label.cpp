#include <gtest/gtest.h>

#include "video/camera_label.hpp"

using mosaic::camera_index_from_video_file;
using mosaic::camera_label;
using mosaic::camera_label_for_video;
using mosaic::camera_short_label;

// On screen, cameras count from 1 — matching the room labels, the camera cards
// and setup_nic_cameras.ps1. Config index 2 is the third camera, "Camera 3".
TEST(CameraLabel, CountsFromOneOnScreen) {
    EXPECT_EQ(camera_label(0), "Camera 1");
    EXPECT_EQ(camera_label(2), "Camera 3");
    EXPECT_EQ(camera_label(5), "Camera 6");
    EXPECT_EQ(camera_short_label(0), "Cam 1");
    EXPECT_EQ(camera_short_label(2), "Cam 3");
}

TEST(CameraLabel, ReadsTheConfigIndexFromAVideoFileName) {
    EXPECT_EQ(camera_index_from_video_file("video_0.mp4"), 0);
    EXPECT_EQ(camera_index_from_video_file("video/video_2.mp4"), 2);
    EXPECT_EQ(camera_index_from_video_file("D:/rec/s1/video/video_10.mkv"), 10);
}

// Same strictness as camera_timestamp_files.hpp: a leftover copy must not be
// mistaken for a camera and given a number it does not have.
TEST(CameraLabel, RejectsNamesThatAreNotCameraVideos) {
    EXPECT_EQ(camera_index_from_video_file("video_2_old.mp4"), -1);
    EXPECT_EQ(camera_index_from_video_file("video_-2.mp4"), -1);
    EXPECT_EQ(camera_index_from_video_file("video_.mp4"), -1);
    EXPECT_EQ(camera_index_from_video_file("anonymized_video_2.mp4"), -1);
    EXPECT_EQ(camera_index_from_video_file("audio.wav"), -1);
}

// The case the label helper exists for: a session missing camera 1 lists its
// videos as [video_0, video_2]. Labelling by list position would call the
// second one "Camera 2"; it is camera 3.
TEST(CameraLabel, LabelsAVideoByItsOwnNumberNotItsPosition) {
    EXPECT_EQ(camera_label_for_video("video/video_2.mp4"), "Camera 3");
}

// Never a made-up number: an unrecognised name is shown as itself.
TEST(CameraLabel, FallsBackToTheFileNameRatherThanInventingANumber) {
    EXPECT_EQ(camera_label_for_video("video/interview_take2.mp4"), "interview_take2.mp4");
}
