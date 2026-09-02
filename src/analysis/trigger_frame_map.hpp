#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <cstdint>

namespace mosaic {

// One camera's nearest-frame result for a single trigger event.
struct TriggerFrameHit {
    int cameraIndex         = -1;
    int frameId             = -1;  // -1 = no timestamps_camN.csv data for this camera
    double deltaMs          = 0.0; // signed: (frame.elapsedNs - trigger.elapsedNs)/1e6
    int64_t videoPositionMs = -1;  // seek() position in that camera's own mp4 PTS domain; -1 = n/a
};

// One trigger.csv row plus its resolved nearest frame in every camera.
struct TriggerFrameRow {
    int rowIndex      = 0;   // 0-based, order in trigger.csv (excluding header)
    int64_t elapsedNs = 0;   // raw, shared elapsed_ns() origin — see TriggerRecorder
    double elapsedMs  = 0.0; // recording-relative, as originally logged; display only
    QString wallClock;
    QString source;
    QString label;
    // Numeric marker from trigger.csv's `code` column (see
    // TriggerEvent::code) — 0 for a file predating that column, and for
    // sources that don't set one.
    int code     = 0;
    double value = 0.0;
    QVector<TriggerFrameHit> frames; // one entry per camera, index == cameraIndex
};

struct TriggerFrameCamera {
    int index = 0;
    QString videoFile; // relative to session dir, e.g. "video/video_0.mp4"
    int framesCaptured = 0;
};

// Resolves every trigger event in a recorded session's trigger.csv to the
// nearest ACTUALLY-CAPTURED frame in every camera — a direct per-camera
// nearest-neighbour lookup against each timestamps_camN.csv, not a resample
// onto SyncManifest's cross-camera uniform master-tick grid (which adds up
// to +-half a tick of quantization on top of native jitter — a different
// problem, "synchronized uniform playback," not "what did camera C actually
// capture nearest instant T"). Pure, fast, deterministic; computed
// synchronously in C++ (not an AnalysisManager subprocess job), mirroring
// how SyncManifest's own generate() is already synchronous C++ despite
// sitting beside the ML-backed analysis plugins.
//
// Requires trigger.csv to have the elapsed_ns column (added alongside this
// class — see TriggerRecorder). Sessions recorded before that fix predate
// the column entirely; generate() returns is_valid() == false for them
// rather than silently reconstructing a value from elapsed_ms against an
// unknown, undiscoverable old TriggerRecorder::startNs offset.
//
// Usage:
//   auto m = TriggerFrameMap::generate(sessionPath);
//   m.save(sessionPath);
//   ...
//   auto m2 = TriggerFrameMap::load(sessionPath);
//   player->seek(m2.row(i).frames[camIdx].videoPositionMs);
class TriggerFrameMap {
   public:
    TriggerFrameMap() = default;

    // ── Factory ───────────────────────────────────────────────────────────
    static TriggerFrameMap generate(const QString& sessionPath);

    // Load existing trigger_frame_map.json from sessionPath.
    static TriggerFrameMap load(const QString& sessionPath);

    // ── Persistence ───────────────────────────────────────────────────────
    bool save(const QString& sessionPath) const;

    // Flat one-row-per-trigger CSV at an arbitrary path (QFileDialog save-as
    // target) — for MNE-Python / external consumption.
    bool export_csv(const QString& csvPath) const;

    // ── Validity ──────────────────────────────────────────────────────────
    [[nodiscard]] bool is_valid() const;

    // ── Queries ───────────────────────────────────────────────────────────
    [[nodiscard]] int camera_count() const;
    [[nodiscard]] int trigger_count() const;
    [[nodiscard]] const TriggerFrameCamera& camera_info(int idx) const;
    [[nodiscard]] const TriggerFrameRow& row(int idx) const;
    [[nodiscard]] const QVector<TriggerFrameRow>& rows() const;

   private:
    bool valid_ = false;
    QString generatedAt_;
    QVector<TriggerFrameCamera> cameras_;
    QVector<TriggerFrameRow> rows_;

    struct FrameTs {
        int frameId       = 0;
        int64_t elapsedNs = 0;
    };
    static QVector<FrameTs> read_timestamps(const QString& csvPath);

    struct RawTrigger {
        int64_t elapsedNs = 0;
        double elapsedMs  = 0.0;
        QString wallClock, source, label;
        int code     = 0;
        double value = 0.0;
    };
    // Quote-aware line split — trigger.csv's label field is "..."-quoted
    // with doubled internal quotes and may itself contain commas (e.g. a
    // free-text stimulus/event marker); a naive split(',') is not safe here.
    static QStringList split_csv_line(const QString& line);

    // Column-order-independent (looks up each column by header name).
    // Sets hasElapsedNsColumn to false (and returns an empty vector) if the
    // file is missing, empty, or predates the elapsed_ns column.
    static QVector<RawTrigger> read_trigger_csv(const QString& csvPath, bool& hasElapsedNsColumn);
};

} // namespace mosaic
