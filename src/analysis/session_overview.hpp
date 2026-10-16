#pragma once
#include <QPointF>
#include <QString>
#include <QVector>

namespace mosaic {

class ConversationResult;
class EyeContactResult;
class FaceDynamicsResult;
class RppgResult;

// ── Session overview ───────────────────────────────────────────────────────
//
// One camera's analyses on a single timeline, so their moments can be read
// against each other (heart rate rising as the person starts to talk, eye
// contact dropping while they think). Built from the result files those
// analyses already wrote; nothing is computed anew. Pure (QtCore), so tested.

/// A stretch of time in one lane. `tag` picks its colour within the lane:
/// who speaks (0 on camera, 1 other), eye contact (0 looking, 1 away).
struct OverviewSpan {
    double startMs = 0.0; ///< From the video's first frame.
    double endMs   = 0.0;
    int tag        = 0;
};

/// One lane: stretches of time, or a line (NaN y breaks it).
struct OverviewLane {
    enum class Kind { Spans, Line };
    QString id;   ///< "speaking", "eye_contact", "smile", "head", "heart_rate"
    QString name; ///< shown at the lane's left
    Kind kind = Kind::Line;
    QVector<OverviewSpan> spans;
    QVector<QPointF> points; ///< x: ms from the video's first frame
    double minY = 0.0;       ///< the lane's vertical range
    double maxY = 1.0;
    QString unit; ///< for the range label, e.g. "bpm"
    /// Why the lane is empty ("Conversation Timing not run"); empty when it
    /// has data.
    QString note;
};

struct SessionOverview {
    double durationMs = 0.0; ///< From the first to the last frame any source saw.
    QVector<OverviewLane> lanes;
};

/// The results available for one camera; a null pointer (or an invalid
/// result) is an analysis not run.
struct OverviewSources {
    const ConversationResult* conversation = nullptr;
    const EyeContactResult* eyeContact     = nullptr;
    const FaceDynamicsResult* faceDynamics = nullptr;
    const RppgResult* rppg                 = nullptr;
};

/// Five lanes, always in this order: who speaks, eye contact, smile, head
/// movement, heart rate. All share one time origin: the earliest first frame
/// among the sources (they are all the same camera's video, so they agree).
[[nodiscard]] SessionOverview build_session_overview(const OverviewSources& sources);

} // namespace mosaic
