#pragma once
// A small 3D Gaze result for the gaze view's tests (the scene's maths in
// mosaic_tests, its page in mosaic_qml_tests): two people across a table
// 1.4 m apart, 2.5 m from the reference camera, 10 analysed frames 80 ms
// apart (ticks 0, 2, ... 18). Frames 0-3: mutual gaze. 4-6: S1 ("Child") at
// the region "screen", S2 at the calibrated plane. 7-9: S1 not seen, S2
// without eyes.
//
// The room frame is the reference camera's (y down): the plane is a table
// top 0.75 m below the cameras, its normal written pointing down (away from
// the cameras), so up must be turned around. Camera 1 looks along -x.
#include <QFile>
#include <QString>
#include <QStringList>

inline QString gaze3d_fixture_json(bool withCameraDirections = true) {
    const auto sample = [](const char* id, const char* name, const QString& origin,
                           const QString& direction, const QString& point, const QString& target,
                           bool mutual) {
        return QString(R"({"id": "%1", "name": "%2", "origin": %3, "direction": %4, )"
                       R"("point": %5, "target": %6, "mutual": %7, "confidence": 0.8, )"
                       R"("uncertainty_deg": 4.0, "n_cameras": 2, "per_camera": []})")
            .arg(id, name, origin, direction, point, target, mutual ? "true" : "false");
    };
    const QString s1 = "[-700, 0, 2500]", s2 = "[700, 0, 2500]";
    QStringList frames;
    for (int k = 0; k < 10; ++k) {
        QStringList people;
        if (k < 4) {
            people << sample("S1", "Child", s1, "[1, 0, 0]", "[700, 0, 2500]",
                             R"({"type": "subject", "label": "S2", "subject": "S2"})", true);
            people << sample("S2", "S2", s2, "[-1, 0, 0]", "[-700, 0, 2500]",
                             R"({"type": "subject", "label": "Child", "subject": "S1"})", true);
        } else if (k < 7) {
            people << sample("S1", "Child", s1, "[0.3, 0, 0.95]", "[0, 0, 3600]",
                             R"({"type": "region", "label": "screen", "subject": null})", false);
            people << sample("S2", "S2", s2, "[0, 0.6, 0.8]", "[700, 750, 3500]",
                             R"({"type": "plane", "label": "plane", "subject": null})", false);
        } else {
            people << sample("S2", "S2", s2, "null", "null", "null", false);
        }
        frames << QString(R"({"tick": %1, "timestamp_ns": %2, "video_frame_index": %1, )"
                          R"("subjects": [%3]})")
                      .arg(2 * k)
                      .arg(80000000LL * k)
                      .arg(people.join(","));
    }
    const QString directions =
        withCameraDirections
            ? QString(R"(, "rotation_room": [[1,0,0],[0,1,0],[0,0,1]], "fov_deg": [60, 40])")
            : QString();
    const QString directions1 =
        withCameraDirections
            ? QString(R"(, "rotation_room": [[0,0,-1],[0,1,0],[1,0,0]], "fov_deg": [60, 40])")
            : QString();
    return QString(R"({"schema": "mosaic-gaze-fusion-v2", "fps": 25.0, "analysed_every": 2, )"
                   R"("cameras": [{"index": 0, "position_room": [0, 0, 0]%1}, )"
                   R"({"index": 1, "position_room": [2500, 0, 2500]%2}], )"
                   R"("plane": {"defined": true, "point": [0, 750, 2500], "normal": [0, 1, 0]}, )"
                   R"("regions": [{"name": "screen", "centre": [0, 0, 3600], )"
                   R"("normal": [0, 0, -1], "u_axis": [1, 0, 0], "width": 600, "height": 340}], )"
                   R"("subjects": [{"id": "S1", "name": "Child"}, {"id": "S2", "name": "S2"}], )"
                   R"("frames": [%3]})")
        .arg(directions, directions1, frames.join(","));
}

// Writes the fixture to `path`; true when written.
inline bool write_gaze3d_fixture(const QString& path, bool withCameraDirections = true) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.write(gaze3d_fixture_json(withCameraDirections).toUtf8());
    return true;
}
