// The 3D room pages (qml/Room3D.qml, qml/Gaze3D.qml), loaded headless with a
// real scene and driven through their views and toggles. Nobody can look at it in CI, and a
// QML mistake (a misspelt property, a binding on something undefined) only
// shows as a warning when the page runs: any warning fails these tests.
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFile>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QStringList>
#include <QTemporaryDir>
#include <QtQml>
#include <memory>

#include "analysis/gaze_fusion_result.hpp"
#include "analysis/skeleton3d_result.hpp"
#include "gaze3d_fixture.hpp"
#include "ui/analysis/gaze3d_scene.hpp"
#include "ui/analysis/room3d_mesh.hpp"
#include "ui/analysis/room3d_scene.hpp"

extern QStringList g_qmlMessages;

namespace {

// Two cameras, one person standing for a second and a half (25 ticks a
// second), with a head, so every part of the page has something to draw.
mosaic::Skeleton3DResult room_result(QTemporaryDir& dir) {
    QStringList frames;
    for (int k = 0; k < 40; ++k) {
        const double x = 20.0 * k;
        frames << QString(R"({"tick": %1, "timestamp_ns": %2, "people": [{"track_id": 0, )"
                          R"("person_id": 1, "num_contributing_cameras": 2, "keypoints_room": [)"
                          R"([%3,-1650,3000],[%4,-1600,3000],[%5,-1600,3000],)"
                          R"([%6,-1400,3000],[%7,-1400,3000],[%6,-900,3000],[%7,-900,3000],)"
                          R"([%6,-80,3000],[%7,-80,3000]], "keypoints_valid": [true,true,true,)"
                          R"(true,true,true,true,true,true]}]})")
                      .arg(k)
                      .arg(40000000LL * k)
                      .arg(x)
                      .arg(x - 80)
                      .arg(x + 80)
                      .arg(x - 200)
                      .arg(x + 200);
    }
    const QString json =
        QString(R"({"schema": "mosaic-skeleton3d-v1", "cameras": [)"
                R"({"index": 0, "position_room": [0, 0, 0], )"
                R"("rotation_room": [[1,0,0],[0,1,0],[0,0,1]], "fov_deg": [60, 40]},)"
                R"({"index": 1, "position_room": [2500, 0, 3000], )"
                R"("rotation_room": [[0,0,-1],[0,1,0],[1,0,0]], "fov_deg": [60, 40]}], )"
                R"("keypoint_names": ["nose", "left_ear", "right_ear", "left_shoulder", )"
                R"("right_shoulder", "left_hip", "right_hip", "left_ankle", "right_ankle"], )"
                R"("skeleton_edges": [[3, 4], [3, 5], [4, 6], [5, 6], [5, 7], [6, 8]], )"
                R"("frames": [%1]})")
            .arg(frames.join(","));
    QFile f(dir.filePath("room.json"));
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(json.toUtf8());
    f.close();
    return mosaic::Skeleton3DResult::load(dir.filePath("room.json"));
}

} // namespace

TEST(Room3DPage, LoadsAndRunsItsViewsWithoutAWarning) {
    QTemporaryDir dir;
    const auto result = room_result(dir);
    ASSERT_TRUE(result.is_valid());

    static const int registered =
        qmlRegisterType<mosaic::SceneMesh>("Mosaic.Room3D", 1, 0, "SceneMesh");
    Q_UNUSED(registered);
    mosaic::Room3DScene scene;
    QQmlEngine engine;
    engine.rootContext()->setContextProperty("room3d", &scene);
    g_qmlMessages.clear();

    QQmlComponent component(&engine, QUrl("qrc:/qml/Room3D.qml"));
    ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
    std::unique_ptr<QObject> page(component.create());
    ASSERT_TRUE(page);
    page->setProperty("width", 900);
    page->setProperty("height", 600);

    // A result, playback, the toggles, every view, and the view from each
    // camera; then no result again.
    scene.set_result(result);
    for (const int64_t ms : {0, 500, 1000}) {
        scene.set_position_ms(ms);
        QCoreApplication::processEvents();
    }
    // The page shows the scene's frame and the stage's room.
    EXPECT_TRUE(page->property("info").toString().contains("1 person(s) shown"));
    EXPECT_TRUE(page->property("stage").value<QObject*>() == &scene.stage());
    EXPECT_GT(page->property("buttonsWidth").toDouble(), 0.0);
    scene.set_trails(false);
    scene.set_paths(true);
    scene.set_follow(1);
    scene.set_position_ms(800);
    scene.set_dyad(1, 2);
    for (int preset = 0; preset < 4; ++preset) {
        page->setProperty("preset", preset);
        QMetaObject::invokeMethod(page.get(), "reset");
        QCoreApplication::processEvents();
    }
    for (int cam = 0; cam < 2; ++cam) {
        page->setProperty("fromCamera", cam);
        scene.set_position_ms(300);
        QCoreApplication::processEvents();
    }
    scene.set_shown(1, false);
    scene.set_result(mosaic::Skeleton3DResult());
    QCoreApplication::processEvents();

    EXPECT_TRUE(g_qmlMessages.isEmpty()) << g_qmlMessages.join("\n").toStdString();
}

TEST(Gaze3DPage, LoadsAndRunsItsViewsWithoutAWarning) {
    QTemporaryDir dir;
    ASSERT_TRUE(write_gaze3d_fixture(dir.filePath("g.json")));
    const auto result = mosaic::GazeFusionResult::load(dir.filePath("g.json"));
    ASSERT_TRUE(result.is_valid());

    static const int registered =
        qmlRegisterType<mosaic::SceneMesh>("Mosaic.Room3D", 1, 0, "SceneMesh");
    Q_UNUSED(registered);
    mosaic::Gaze3DScene scene;
    QQmlEngine engine;
    engine.rootContext()->setContextProperty("gaze3d", &scene);
    g_qmlMessages.clear();

    QQmlComponent component(&engine, QUrl("qrc:/qml/Gaze3D.qml"));
    ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
    std::unique_ptr<QObject> page(component.create());
    ASSERT_TRUE(page);
    page->setProperty("width", 900);
    page->setProperty("height", 600);

    // A result with its 3D Pose bodies, playback through mutual gaze, the
    // targets lit, a person missing; the toggles, every view, the view from
    // each camera; then no result again.
    ASSERT_TRUE(write_gaze3d_pose_fixture(dir.filePath("pose.json")));
    const auto pose = mosaic::Skeleton3DResult::load(dir.filePath("pose.json"));
    ASSERT_TRUE(pose.is_valid());
    scene.set_result(result, &pose);
    for (const int64_t ms : {0, 400, 640}) {
        scene.set_position_ms(ms);
        QCoreApplication::processEvents();
    }
    EXPECT_TRUE(page->property("info").toString().contains("person(s) shown"));
    scene.set_trails(false);
    scene.set_show_bodies(false);
    QCoreApplication::processEvents();
    scene.set_show_bodies(true);
    scene.set_follow("S2");
    scene.set_position_ms(400);
    for (int preset = 0; preset < 4; ++preset) {
        page->setProperty("preset", preset);
        QMetaObject::invokeMethod(page.get(), "reset");
        QCoreApplication::processEvents();
    }
    for (int cam = 0; cam < 2; ++cam) {
        page->setProperty("fromCamera", cam);
        scene.set_position_ms(0);
        QCoreApplication::processEvents();
    }
    scene.set_shown("S1", false);
    scene.set_result(mosaic::GazeFusionResult());
    QCoreApplication::processEvents();

    EXPECT_TRUE(g_qmlMessages.isEmpty()) << g_qmlMessages.join("\n").toStdString();
}
