#pragma once
#include <QString>
#include <functional>
#include <vector>

namespace mosaic {

// Making a camera's synced video from Frame Sync's alignment.
//
// Frame Sync (analysis/run_sync_repair.py) writes, per camera, a repair map:
// output frame k shows source frame `source_frame_index` of the original
// video, or repeats the last one where the camera has no frame for that
// tick. This turns that map into synced/video_N.mp4 with MOSAIC's own
// encoder (NVENC, libx264 without a GPU) at the original's bitrate, so the
// copy is about the original's size and is written faster than real time.
// A repeated frame carries a red MISSING tag in its top-left corner.
// See docs/math/frame_sync.rst.

/// One output frame of a repair map.
struct RepairMapRow {
    int outputIndex = 0;     ///< frame of the synced video
    int sourceIndex = -1;    ///< frame of the original video it shows (decode order)
    bool missing    = false; ///< the camera has no frame for this tick: a repeat, tagged
};

/// Reads synced/video_N.repair_map.csv. Empty, with `error` set, when the
/// file cannot be read or is from before this export (no
/// source_frame_index column: run Frame Sync again).
[[nodiscard]] std::vector<RepairMapRow> read_repair_map(const QString& csvPath, QString* error);

struct SyncedExportResult {
    bool ok     = false;
    int frames  = 0; ///< frames written
    int missing = 0; ///< of which tagged MISSING (including past a cut-off source)
    QString encoder; ///< "h264_nvenc" or "libx264"
    QString error;
};

/// Writes `outPath` from `sourceVideo` following `rows`, at `fps` frames
/// per second. It is written to a ".partial.mp4" beside it and renamed when
/// complete, so a cut-off export never leaves a short video behind.
/// `progress(done, total)` is called now and then; returning false stops
/// the export (the partial file is removed). Needs FFmpeg: without it the
/// result says so.
[[nodiscard]] SyncedExportResult export_synced_video(
    const QString& sourceVideo, const std::vector<RepairMapRow>& rows, double fps,
    const QString& outPath, const std::function<bool(int, int)>& progress = {});

} // namespace mosaic
