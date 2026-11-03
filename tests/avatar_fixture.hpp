#pragma once
// A tiny 3D Avatar result for the avatar tests (AvatarBody in mosaic_tests,
// the room page in mosaic_qml_tests): one person, a body of three vertices
// in one triangle (in the z = 0 plane, wound so its normal is +z), two bones
// ("root" and "lowerleg01.L") and two ticks 40 ms apart (from 1000 ms by
// default), laid out as analysis/avatar/io.py writes it.
//
// Tick 0: bone 0 at rest, bone 1 moved +2 along x. Tick 1: bone 0 turned 90
// degrees about z, bone 1 at rest, and the left ankle not found.
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <cstdint>

#include "analysis/avatar_result.hpp"

inline mosaic::AvatarResult::Person tiny_avatar_person() {
    mosaic::AvatarResult::Person p;
    p.personId         = 1;
    p.file             = "avatar/person_1.bin";
    p.vertices         = 3;
    p.faces            = 1;
    p.weightsPerVertex = 2;
    p.ticks            = 2;
    p.boneCount        = 2;
    return p;
}

// Writes the person's file at `path`, `dropBytes` short of its size.
inline QString write_tiny_avatar(const QString& path, int dropBytes = 0, double firstMs = 1000.0) {
    const float rest[9]     = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const uint32_t faces[3] = {0, 1, 2};
    const uint16_t bones[6] = {0, 1, 0, 1, 1, 0}; // 2 per vertex
    const float weights[6]  = {1, 0, 0.5f, 0.5f, 1, 0};
    const double time[2]    = {firstMs, firstMs + 40.0};
    const uint32_t valid[2] = {0x1FFFF, 0x1FFFF & ~(1U << 15)};
    // Per tick and bone: x y z w, tx ty tz.
    const float transforms[28] = {0, 0, 0, 1, 0,           0,           0, /**/ 0, 0, 0,      1, 2,
                                  0, 0, 0, 0, 0.70710678f, 0.70710678f, 0, 0,      0, /**/ 0, 0, 0,
                                  1, 0, 0, 0};
    QByteArray data;
    const auto add = [&](const void* p, size_t n) {
        data.append(static_cast<const char*>(p), static_cast<qsizetype>(n));
    };
    add(rest, sizeof rest);
    add(faces, sizeof faces);
    add(bones, sizeof bones);
    add(weights, sizeof weights);
    add(time, sizeof time);
    add(valid, sizeof valid);
    add(transforms, sizeof transforms);
    data.chop(dropBytes);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(data);
    f.close();
    return path;
}

// The whole result in a session folder: avatar/avatar.json (the fit's frame
// the room's, so 1 m is 1000 room mm) and the person's file, for the 3D
// Pose person `personId`.
inline void write_tiny_avatar_session(const QString& sessionPath, int personId = 1,
                                      double firstMs = 1000.0) {
    write_tiny_avatar(sessionPath + "/avatar/person_1.bin", 0, firstMs);
    QFile json(sessionPath + "/avatar/avatar.json");
    json.open(QIODevice::WriteOnly);
    json.write(QString(R"({"schema": "mosaic-avatar-v1", "to_room": [[1,0,0],[0,1,0],[0,0,1]],
        "bone_labels": ["root", "lowerleg01.L"],
        "people": [{"person_id": %1, "file": "avatar/person_1.bin", "vertices": 3,
                    "faces": 1, "weights_per_vertex": 2, "ticks": 2, "bone_count": 2}]})")
                   .arg(personId)
                   .toUtf8());
    json.close();
}
