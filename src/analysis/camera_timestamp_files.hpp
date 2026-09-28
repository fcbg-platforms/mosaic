#pragma once
#include <QDir>
#include <QString>
#include <QVector>

namespace mosaic {

// Finding a session's per-camera timestamp CSVs, by camera number rather than
// by position.
//
// Both SyncManifest::generate() and TriggerFrameMap::build() used to walk
// `timestamps_cam0.csv, cam1.csv, …` and stop at the first missing file, then
// number their output by position in that contiguous run. That holds only while
// no camera is absent — and a camera is absent more often than it sounds: one
// that failed to open, one deliberately not recorded, or a session recorded with
// a single camera that is not camera 0. In every one of those cases the scan
// stopped early and the manifest came out empty or misnumbered, taking session
// playback alignment and the cross-camera analysis plugins with it.
//
// The fix is to enumerate what is actually on disk and keep each camera's real
// number. `analysis_manager.cpp` already globs these files the same way to
// decide whether a manifest is stale; this is that approach, shared.
//
// QtCore-only, so mosaic_tests can cover it.

/// The N in "timestamps_camN.csv", or -1 when the name is not one.
///
/// Deliberately strict: the digits must be all that sits between the prefix and
/// the extension, so "timestamps_cam2.csv" parses and "timestamps_cam2_old.csv"
/// or "timestamps_camX.csv" do not. A session folder is not a controlled
/// environment — a backup copy or an editor's leftover must not be read as a
/// camera and silently join the manifest.
[[nodiscard]] int parse_camera_timestamp_index(const QString& fileName);

/// Camera numbers that have a timestamp CSV in `videoDir`, ascending.
///
/// Ascending because everything downstream reports cameras in order and a
/// directory listing's order is not guaranteed to be numeric — "cam10" sorts
/// before "cam2" as text.
///
/// Returns the numbers, not the paths: the caller still has to read each file
/// and may legitimately drop one that turns out to be empty, and pairing a path
/// back to its number afterwards is exactly the mistake this module exists to
/// stop.
[[nodiscard]] QVector<int> discover_camera_timestamp_indices(const QDir& videoDir);

} // namespace mosaic
