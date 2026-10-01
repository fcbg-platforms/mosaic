#pragma once
#include <QFileInfo>
#include <QString>

namespace mosaic {

// What an operator sees a camera called, everywhere.
//
// Cameras are numbered from 1 on screen and from 0 in data. The data side is
// the configured index: video_2.mp4, timestamps_cam2.csv, "cam2_frame_ids" in
// sync_manifest.json, the "index" fields, and log lines like "[Camera 2]". Those
// are contracts that saved sessions, the Python plugins and existing files rely
// on, so they stay 0-based.
//
// Everything a person reads stays 1-based, matching the room labels, the camera
// cards and setup_nic_cameras.ps1. Before this existed the app was split down
// the middle: the Live view, camera cards, session player and health dialog
// said "Cam 3" for config index 2, while the Analysis tab, calibration, the
// Real-time tab and the 3D room views called the same camera "Camera 2". That is
// how someone ends up reseating the wrong cable.
//
// Every label takes the camera's *configured index*, never a position in some
// list. The two diverge as soon as a camera is missing, and labelling by
// position names the wrong camera on exactly the sessions that need care.
//
// Header-only and QtCore-only, so mosaic_tests covers it.

/// "Camera 3" for config index 2.
[[nodiscard]] inline QString camera_label(int configIndex) {
    return QStringLiteral("Camera %1").arg(configIndex + 1);
}

/// "Cam 3" for config index 2 — for tiles, column headers and drawn overlays.
[[nodiscard]] inline QString camera_short_label(int configIndex) {
    return QStringLiteral("Cam %1").arg(configIndex + 1);
}

/// The configured index a recorded video belongs to, from its file name:
/// "video_2.mp4" or "video/video_2.mp4" -> 2. -1 when the name is not one.
///
/// Strict for the same reason camera_timestamp_files.hpp is: only digits
/// between "video_" and the extension, so "video_2_old.mp4" or "video_-2.mp4"
/// are not read as cameras. Any extension is accepted; SessionInfo lists
/// mp4, mkv and avi.
[[nodiscard]] inline int camera_index_from_video_file(const QString& path) {
    const QString base = QFileInfo(path).completeBaseName();
    const QString prefix(QStringLiteral("video_"));
    if (!base.startsWith(prefix) || base.size() == prefix.size()) {
        return -1;
    }
    const QString digits = base.mid(prefix.size());
    for (const QChar c : digits) {
        if (!c.isDigit()) {
            return -1;
        }
    }
    bool ok       = false;
    const int idx = digits.toInt(&ok);
    return ok ? idx : -1;
}

/// camera_label() for a recorded video, or the bare file name when it does not
/// follow the video_N convention — never a made-up number.
[[nodiscard]] inline QString camera_label_for_video(const QString& path) {
    const int idx = camera_index_from_video_file(path);
    return idx >= 0 ? camera_label(idx) : QFileInfo(path).fileName();
}

} // namespace mosaic
