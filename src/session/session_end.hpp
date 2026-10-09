#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <cstdint>
#include <vector>

namespace mosaic {

// Whether a recording finished, is still running, or was cut short.
//
// session_meta.json is written when a recording starts, so on its own it says
// nothing about how the recording ended. RecordManager writes it with
// "session_end": null and replaces that with an object once every subsystem
// has stopped. A null alone is not enough, though: it also describes a session
// that is recording *right now* — in this process, or on another machine
// sharing the recordings folder — and calling that "interrupted" would tell
// an operator mid-session that MOSAIC had crashed.
//
// So while recording, RecordManager also rewrites a small heartbeat file in
// the session folder every kHeartbeatIntervalMs, holding the current UTC time,
// and deletes it on a clean stop. A null end with a recent heartbeat is a live
// recording; with a stale or missing one, the recording never finished.
// The time is read from the file's *contents*, not its modification time:
// NTFS can update a file's last-write time lazily, and comparing contents
// works the same from any machine whose clock is roughly right.
//
// QtCore-only, so mosaic_tests covers it.

enum class SessionEnd {
    // No "session_end" key: recorded before this existed. Nothing is known,
    // so nothing is claimed — such sessions are not flagged.
    Unknown,
    // Not finished, but its heartbeat is recent: being recorded now.
    Recording,
    // Not finished and no recent heartbeat: MOSAIC crashed, was killed or
    // the machine went down; its files end wherever that left them.
    Interrupted,
    // Stopped normally.
    Clean,
};

inline constexpr const char* kHeartbeatFileName = "recording_heartbeat";
inline constexpr int kHeartbeatIntervalMs       = 5000;
// Several missed beats, so a briefly stalled GUI thread (a blocking dialog, a
// slow disk) is not mistaken for a crash.
inline constexpr int kHeartbeatStaleSec = 20;

/// @param lastHeartbeatUtc read_heartbeat()'s result; invalid when none.
[[nodiscard]] SessionEnd session_end_state(const QJsonObject& sessionMeta,
                                           const QDateTime& lastHeartbeatUtc,
                                           const QDateTime& nowUtc);

/// The heartbeat time in `sessionPath`, or an invalid QDateTime if there is
/// no readable one.
[[nodiscard]] QDateTime read_heartbeat(const QString& sessionPath);

/// Writes `nowUtc` as the heartbeat (atomically, via QSaveFile).
bool write_heartbeat(const QString& sessionPath, const QDateTime& nowUtc);

/// Deletes the heartbeat — part of a clean stop.
void remove_heartbeat(const QString& sessionPath);

/// What every session list says about an interrupted session, so the wording
/// is the same wherever one is shown.
inline constexpr const char* kInterruptedSessionTip =
    "MOSAIC stopped without finishing this recording (crash, forced close or power loss). "
    "Its files end where that happened; the last couple of seconds may be missing, and the "
    "cameras' videos and timestamp files may end a second or two apart.";

/// What one camera actually recorded, measured from the frames written to its
/// video file (VideoManager::RecordingCameraSnapshot).
struct RecordedCamera {
    int index              = 0; ///< configured index, as in "cameras"[].index
    int64_t frames         = 0;
    int64_t firstElapsedNs = -1; ///< -1 when no frame was written
    int64_t lastElapsedNs  = -1;
};

/// The rate a camera's video really has: frame intervals over the time from
/// its first frame to its last, so a camera that started late or stopped
/// early is judged on the time it was recording, not on the session's length.
/// A gap (a camera that dropped out and came back) lowers it, as it should.
/// -1 when it cannot be known: fewer than two frames, or no time between them.
[[nodiscard]] double achieved_fps(int64_t frames, int64_t firstElapsedNs, int64_t lastElapsedNs);

/// Records a normal stop in `sessionPath`/session_meta.json: sets
/// "session_end" to {"utc", "duration_ms", "ended_cleanly": true}, keeping
/// every other field as it was.
///
/// Each of `cameras` is added to the "cameras" entry with the same index as
/// "frames_recorded", "recorded_seconds" (first frame to last) and
/// "achieved_fps" (see achieved_fps(); left out when unknown). "fps" says what
/// was asked for and "camera_reported_fps" what the camera promised at the
/// start; this is what the video holds. A configured camera with no entry
/// (never opened) gets none of these keys.
///
/// Written through QSaveFile, so the file is either the old one or the new
/// one — a crash or full disk partway through can never leave a truncated
/// session_meta.json, which would lose the session's identity and camera list
/// along with the end marker. Returns false (and leaves the file untouched) if
/// it cannot be read, parsed or replaced.
bool mark_session_ended(const QString& sessionPath, qint64 durationMs, const QDateTime& endUtc,
                        const std::vector<RecordedCamera>& cameras = {});

} // namespace mosaic
