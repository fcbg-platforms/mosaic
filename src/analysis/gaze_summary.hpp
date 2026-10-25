#pragma once
#include <QString>
#include <QVector>
#include <cstdint>

#include "analysis/gaze_fusion_result.hpp"

namespace mosaic {

/// A subject's gaze landing on no target: a gaze was found, and went past
/// every face, region and the plane. Its key and its label.
inline const QString kGazeNothing = QStringLiteral("nothing");
/// A subject seen, with no usable eyes: no gaze at that tick. Its key and
/// its label.
inline const QString kGazeNoGaze = QStringLiteral("no gaze");

/// What a subject looks at, as the charts tell targets apart: by `key`
/// ("subject/S2", "region/screen", "plane", kGazeNothing, kGazeNoGaze), so
/// a person and a region with the same name stay two; `label` is the name
/// shown (the looked-at person's name, the region's).
struct GazeTarget {
    QString key;
    QString label;
    /// A face, a region or the plane; false for nothing and no gaze.
    [[nodiscard]] bool is_target() const { return key != kGazeNothing && key != kGazeNoGaze; }
};

/// One stretch of time: a subject looking at one target, or a pair in
/// mutual gaze.
struct GazeSpan {
    GazeTarget target;   ///< what was looked at (empty for a mutual gaze episode)
    int64_t startNs = 0; ///< the first tick's time
    int64_t endNs   = 0; ///< the last tick's time, plus one tick's step
};

/// The time a subject spent on one target.
struct GazeTargetTime {
    GazeTarget target;
    double seconds = 0.0;
};

/// What one subject's gaze did over the recording.
struct GazeSubjectSummary {
    QString id;
    QString name;
    double secondsSeen = 0.0; ///< analysed ticks with this subject, times the step
    /// Seconds on each target (nothing and no gaze included), longest first.
    QVector<GazeTargetTime> secondsByTarget;
    QVector<GazeSpan> spans; ///< in time order
};

/// Two subjects' mutual gaze: each looking at the other's face.
struct GazePairSummary {
    QString a, b; ///< ids, a before b in the result's order
    QString nameA, nameB;
    double seconds  = 0.0;
    double longestS = 0.0;   ///< the longest stretch
    QVector<GazeSpan> spans; ///< each an episode, in time order
};

/// The whole recording's gaze, for the 3D Gaze charts and their export.
struct GazeSummary {
    double stepS = 0.0; ///< how long one analysed tick stands for
    QVector<GazeSubjectSummary> subjects;
    QVector<GazePairSummary> pairs; ///< every pair of subjects; none with one
};

/// Sums up a gaze result: per subject, the time on each target and the
/// stretches of looking at it; per pair, the mutual gaze.
///
/// Each analysed tick stands for the step between analysed ticks (the
/// median gap, so with --skip 2 two ticks), so the seconds add up to the
/// time the subject was seen. Ticks of one target further apart than one
/// and a half steps (the subject not seen in between) start a new stretch:
/// never across a gap.
[[nodiscard]] GazeSummary summarize_gaze(const GazeFusionResult& result);

} // namespace mosaic
