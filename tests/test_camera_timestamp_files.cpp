#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

#include "analysis/camera_timestamp_files.hpp"

using mosaic::discover_camera_timestamp_indices;
using mosaic::parse_camera_timestamp_index;

namespace {

void write_csv(const QDir& dir, const QString& name) {
    QFile f(dir.filePath(name));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream(&f) << "frame_id,elapsed_ns,wall_ns,hw_timestamp_ns\n";
}

} // namespace

// ── parse_camera_timestamp_index ───────────────────────────────────────────

TEST(CameraTimestampFiles, ParsesTheCameraNumber) {
    EXPECT_EQ(parse_camera_timestamp_index("timestamps_cam0.csv"), 0);
    EXPECT_EQ(parse_camera_timestamp_index("timestamps_cam2.csv"), 2);
    EXPECT_EQ(parse_camera_timestamp_index("timestamps_cam10.csv"), 10);
}

// A session folder is not a controlled environment. A backup copy or an
// editor's leftover must not be read as a camera and silently join the
// manifest as an extra one.
TEST(CameraTimestampFiles, RejectsAnythingThatIsNotExactlyACameraFile) {
    EXPECT_EQ(parse_camera_timestamp_index("timestamps_cam2_old.csv"), -1);
    EXPECT_EQ(parse_camera_timestamp_index("timestamps_cam2.csv.bak"), -1);
    EXPECT_EQ(parse_camera_timestamp_index("timestamps_camX.csv"), -1);
    EXPECT_EQ(parse_camera_timestamp_index("timestamps_cam.csv"), -1);
    EXPECT_EQ(parse_camera_timestamp_index("trigger.csv"), -1);
    EXPECT_EQ(parse_camera_timestamp_index(""), -1);
    EXPECT_EQ(parse_camera_timestamp_index("timestamps_cam2.CSV"), -1); // case-sensitive
}

// QString::toInt() accepts leading whitespace and a sign, so a bare toInt()
// here would turn " 2" and "-2" into cameras. Require pure digits.
TEST(CameraTimestampFiles, RejectsSignedAndPaddedNumbers) {
    EXPECT_EQ(parse_camera_timestamp_index("timestamps_cam-2.csv"), -1);
    EXPECT_EQ(parse_camera_timestamp_index("timestamps_cam+2.csv"), -1);
    EXPECT_EQ(parse_camera_timestamp_index("timestamps_cam 2.csv"), -1);
}

// Leading zeros are digits and parse to the same camera. Nothing writes these
// today, but reading one as -1 would silently drop a camera.
TEST(CameraTimestampFiles, LeadingZerosStillNameACamera) {
    EXPECT_EQ(parse_camera_timestamp_index("timestamps_cam02.csv"), 2);
}

// ── discover_camera_timestamp_indices ──────────────────────────────────────

TEST(CameraTimestampFiles, FindsNothingInAnEmptyDirectory) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    EXPECT_TRUE(discover_camera_timestamp_indices(QDir(dir.path())).isEmpty());
}

TEST(CameraTimestampFiles, FindsAContiguousRun) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir d(dir.path());
    for (int i = 0; i < 5; ++i) {
        write_csv(d, QString("timestamps_cam%1.csv").arg(i));
    }
    EXPECT_EQ(discover_camera_timestamp_indices(d), (QVector<int>{0, 1, 2, 3, 4}));
}

// The case that motivated all of this: a single-camera session recorded from
// camera 2. The old contiguous scan stopped at the missing cam0 and reported
// nothing at all.
TEST(CameraTimestampFiles, FindsASingleCameraThatIsNotCameraZero) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir d(dir.path());
    write_csv(d, "timestamps_cam2.csv");
    EXPECT_EQ(discover_camera_timestamp_indices(d), (QVector<int>{2}));
}

// A gap in the middle — one camera failed to open. The old scan truncated at
// the gap, so cameras after it vanished from the manifest entirely.
TEST(CameraTimestampFiles, FindsCamerasEitherSideOfAGap) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir d(dir.path());
    write_csv(d, "timestamps_cam0.csv");
    write_csv(d, "timestamps_cam1.csv");
    write_csv(d, "timestamps_cam3.csv");
    write_csv(d, "timestamps_cam5.csv");
    EXPECT_EQ(discover_camera_timestamp_indices(d), (QVector<int>{0, 1, 3, 5}));
}

// Numeric order, not the listing's lexicographic order — "cam10" sorts before
// "cam2" as text. Without the explicit sort, a rig's camera ordering would
// depend on how many cameras it happens to have.
TEST(CameraTimestampFiles, ReturnsNumericOrderNotTextOrder) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir d(dir.path());
    for (const int i : {10, 2, 0, 11, 1}) {
        write_csv(d, QString("timestamps_cam%1.csv").arg(i));
    }
    EXPECT_EQ(discover_camera_timestamp_indices(d), (QVector<int>{0, 1, 2, 10, 11}));
}

// The session folder holds other CSVs and stray files; none of them is a camera.
TEST(CameraTimestampFiles, IgnoresEverythingElseInTheFolder) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir d(dir.path());
    write_csv(d, "timestamps_cam1.csv");
    write_csv(d, "trigger.csv");
    write_csv(d, "timestamps_cam1.csv.bak");
    write_csv(d, "timestamps_camX.csv");
    ASSERT_TRUE(QDir(d).mkdir("timestamps_cam9.csv")); // a directory, not a file
    EXPECT_EQ(discover_camera_timestamp_indices(d), (QVector<int>{1}));
}
