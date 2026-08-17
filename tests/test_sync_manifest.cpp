#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <tuple>

#include "analysis/sync_manifest.hpp"

using mosaic::SyncManifest;

namespace {

// Writes a timestamps_camN.csv with the given (frame_id, elapsed_ns, wall_ns) rows.
void write_timestamps_csv(const QString& path,
                          const QVector<std::tuple<int, int64_t, int64_t>>& rows) {
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream ts(&f);
    ts << "frame_id,elapsed_ns,wall_ns\n";
    for (const auto& [frameId, elapsedNs, wallNs] : rows) {
        ts << frameId << ',' << elapsedNs << ',' << wallNs << '\n';
    }
}

} // namespace

// Two cameras whose frames land on identical elapsed_ns ticks, but whose
// wall_ns columns diverge partway through — camera 1 simulates an NTP step
// (a multi-second jump) partway through the session. Under the old wall_ns-
// based alignment this would have produced large per-tick errors and poor
// coverage for camera 1. Alignment must now be immune to it, since it uses
// the monotonic elapsed_ns clock shared by all grab threads.
TEST(SyncManifest, AlignmentIgnoresWallClockJump) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const int64_t stepNs = 40'000'000LL; // 40 ms = 25 fps master tick

    QVector<std::tuple<int, int64_t, int64_t>> cam0, cam1;
    const int64_t wallBase = 1'700'000'000'000'000'000LL; // arbitrary epoch offset
    for (int i = 0; i < 5; ++i) {
        const int64_t elapsedNs = static_cast<int64_t>(i) * stepNs;
        cam0.append({i, elapsedNs, wallBase + elapsedNs});

        // Camera 1: identical elapsed_ns sequence, but wall_ns jumps forward
        // by 5 seconds starting at frame 2 (simulated NTP step).
        const int64_t jump = (i >= 2) ? 5'000'000'000LL : 0LL;
        cam1.append({i, elapsedNs, wallBase + elapsedNs + jump});
    }

    ASSERT_TRUE(QDir().mkpath(dir.path() + "/video"));
    const QDir videoDir(dir.path() + "/video");
    write_timestamps_csv(videoDir.filePath("timestamps_cam0.csv"), cam0);
    write_timestamps_csv(videoDir.filePath("timestamps_cam1.csv"), cam1);

    const SyncManifest m = SyncManifest::generate(dir.path(), /*masterFps=*/25.0);

    ASSERT_TRUE(m.is_valid());
    EXPECT_EQ(m.camera_count(), 2);
    EXPECT_EQ(m.total_ticks(), 5);
    EXPECT_EQ(m.t_origin_ns(), 0);

    for (int cam = 0; cam < 2; ++cam) {
        const auto& info = m.camera_info(cam);
        EXPECT_DOUBLE_EQ(info.coveragePct, 100.0)
            << "camera " << cam << " should have full coverage regardless of wall_ns jump";
        EXPECT_NEAR(info.meanDeltaMs, 0.0, 1e-6) << "camera " << cam;
        EXPECT_NEAR(info.maxDeltaMs, 0.0, 1e-6) << "camera " << cam;
        EXPECT_NEAR(info.fpsActual, 25.0, 1e-6) << "camera " << cam;
        EXPECT_EQ(info.seekOffsetMs, 0) << "camera " << cam;

        for (int tick = 0; tick < 5; ++tick) {
            EXPECT_EQ(m.frame_at_tick(cam, tick), tick);
            EXPECT_NEAR(m.delta_ms_at_tick(cam, tick), 0.0, 1e-6);
        }
    }
}

// A camera whose frames are offset by a fixed sub-tick amount in elapsed_ns
// should show that offset as a consistent per-tick delta, not zero — proving
// the delta computation still measures real timing error correctly.
TEST(SyncManifest, DetectsGenuineElapsedNsOffset) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const int64_t stepNs = 40'000'000LL; // 25 fps
    const int64_t offset = 5'000'000LL;  // 5 ms late, every frame

    QVector<std::tuple<int, int64_t, int64_t>> cam0, cam1;
    for (int i = 0; i < 5; ++i) {
        const int64_t e0 = static_cast<int64_t>(i) * stepNs;
        cam0.append({i, e0, e0});
        cam1.append({i, e0 + offset, e0 + offset});
    }

    ASSERT_TRUE(QDir().mkpath(dir.path() + "/video"));
    const QDir videoDir(dir.path() + "/video");
    write_timestamps_csv(videoDir.filePath("timestamps_cam0.csv"), cam0);
    write_timestamps_csv(videoDir.filePath("timestamps_cam1.csv"), cam1);

    const SyncManifest m = SyncManifest::generate(dir.path(), /*masterFps=*/25.0);
    ASSERT_TRUE(m.is_valid());

    // t_origin = max(front elapsed_ns) = cam1's first frame (offset ahead).
    EXPECT_EQ(m.t_origin_ns(), offset);

    // Camera 1 defines the origin, so its own delta at every tick is 0.
    const auto& cam1Info = m.camera_info(1);
    EXPECT_NEAR(cam1Info.meanDeltaMs, 0.0, 1e-6);

    // Camera 0 is consistently `offset` behind t_origin at every tick.
    const auto& cam0Info = m.camera_info(0);
    EXPECT_NEAR(cam0Info.meanDeltaMs, offset / 1e6, 1e-3);
}
