#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

namespace mosaic {

// What is checked when Record is clicked, and what each check concluded.
//
// Most of what has gone wrong on this rig was only found *after* a recording:
// a camera that never opened, cameras running well under their target rate,
// Action1 sync silently falling back to free-run. All of it is knowable the
// moment Record is clicked, and so is the one thing that has not happened yet
// but would cost a whole session — a disk that fills up partway through.
//
// Same split as session_health.hpp: this decides what is true from plain
// data; MainWindow gathers the data from the managers, and
// SessionIdentityDialog words and colours it. Nothing here blocks a
// recording — every finding can be overridden with "Record anyway", because a
// session that has to happen now is better recorded with a known problem than
// not recorded at all.
//
// QtCore-only, so mosaic_tests covers it.

/// Less recording time than this left on the disk is a certain loss.
inline constexpr double kPreflightDiskFailMinutes = 10.0;
/// Less than this is worth a warning: a long session would not fit.
inline constexpr double kPreflightDiskWarnMinutes = 60.0;

/// A camera whose newest frame is older than this has stopped delivering,
/// even though it delivered once. Generous next to any rate this rig runs
/// at (a frame every 40-70 ms), so a slow frame is never mistaken for a
/// stalled link.
inline constexpr double kPreflightStaleFrameSec = 3.0;

/// One configured camera that the session is expected to record.
struct PreflightCamera {
    int configIndex     = 0;
    bool opened         = false;
    bool grabberRunning = false;
    // Seconds since its newest frame arrived; < 0 when none ever has. An age,
    // not "a frame arrived": a link that drops mid-preview leaves the last
    // timestamp in place, and only its age shows the stream has stopped.
    double lastFrameAgeSec = -1.0;
    // Configured for GigE Action1 triggering, and actually in the group.
    bool wantsAction1 = false;
    bool action1Ready = false;
    // Whether the operator pinned a rate (CameraParameters::specifyFps).
    // Without one the camera runs as fast as it can and configuredFps is an
    // unused field — comparing against it would warn on every click.
    bool fixedRate       = true;
    double configuredFps = 0.0;
    // The camera's own measured rate; <= 0 means not measured yet (it needs a
    // few seconds of grabbing — see VideoGrabber::achievable_fps()).
    double achievableFps = -1.0;
};

struct PreflightInput {
    bool videoEnabled = true;
    bool audioEnabled = true;
    QVector<PreflightCamera> cameras;
    int configuredMics = 0;
    // Display names of configured microphones whose device is not present.
    QStringList missingMics;
    // Free bytes on the recordings volume; < 0 when it could not be read.
    qint64 freeBytes = -1;
    // Estimated bytes per second the recording will write.
    double bytesPerSec = 0.0;
    // Shown in the disk row, so the operator knows which drive is meant.
    QString directory;
    // The bitrate behind bytesPerSec is a guess rather than a setting (CPU
    // encoder at constant quality), so the disk figure is worded as rough.
    bool rateIsRough = false;
};

enum class PreflightLevel { Ok, Warn, Fail };

struct PreflightItem {
    PreflightLevel level = PreflightLevel::Ok;
    QString title;
    QString detail;
};

struct PreflightReport {
    // Fails first, then warnings, then the checks that passed.
    QVector<PreflightItem> items;

    [[nodiscard]] bool needs_attention() const;
    [[nodiscard]] PreflightLevel worst() const;
    /// "title — detail" for every warning and failure, in report order. What
    /// session_meta.json records when the operator records anyway.
    [[nodiscard]] QStringList problems() const;
};

[[nodiscard]] PreflightReport evaluate_preflight(const PreflightInput& in);

/// Rough bytes per second a recording writes: every camera at the video
/// bitrate, plus every microphone as 16-bit PCM — or a quarter of that for a
/// compressed codec, which is generous for FLAC and pessimistic for AAC/MP3.
/// Container overhead is ignored; it is small next to the margin the disk
/// thresholds leave.
[[nodiscard]] double estimate_recording_bytes_per_sec(int cameras, int videoKbps, int mics,
                                                      int sampleRate, int channels,
                                                      const QString& audioCodec);

/// Free bytes on the volume `directory` would be created on, or -1. The
/// recordings folder may not exist yet (per-user folders are created by the
/// first recording), so the nearest existing ancestor is measured instead.
[[nodiscard]] qint64 free_bytes_for(const QString& directory);

} // namespace mosaic
