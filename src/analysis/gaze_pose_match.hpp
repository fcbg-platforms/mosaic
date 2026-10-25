#pragma once
#include <QString>
#include <QTextStream>
#include <QVector>

#include "analysis/gaze_fusion_result.hpp"
#include "analysis/skeleton3d_result.hpp"

namespace mosaic {

/// Which 3D Pose person a 3D Gaze subject is.
struct GazePoseMatch {
    QString subject;         ///< the gaze subject's id ("S1")
    int person         = -1; ///< the 3D Pose person (1, 2, ...); -1: none
    double medianMm    = 0;  ///< median distance between their heads, over the shared ticks
    double shareWithin = 0;  ///< share of the shared ticks with the heads within the limit
    int sharedTicks    = 0;
    int agreeingTicks  = 0; ///< shared ticks with the heads within the limit
};

/// Matches each gaze subject to a 3D Pose person, by where their heads are.
///
/// At each gaze tick, the 3D Pose frame closest in time (within 50 ms) is
/// taken; a person's head is the mean of their seen face points (nose,
/// eyes, ears), the subject's its gaze origin (between the eyes), both in
/// the room frame. A pair is a candidate when they share at least 5 ticks,
/// the heads are within `withinMm` on at least `share` of them, and the
/// median distance is within `withinMm` too. Candidates are taken by the
/// number of ticks their heads agree on, most first (then the closest), each
/// subject and person once: a person seen for a moment beside the subject
/// (a split track) does not win over the one seen with them all along. A
/// subject without one is listed with
/// person -1: two people sitting close, or few cameras, are left unmatched
/// rather than guessed.
[[nodiscard]] QVector<GazePoseMatch> match_gaze_to_pose(const GazeFusionResult& gaze,
                                                        const Skeleton3DResult& pose,
                                                        double withinMm = 250.0,
                                                        double share    = 0.5);

/// Everyone in a 3D Pose result, tick by tick, as people.csv: per person in
/// the frame, the one-person measures (person_csv_columns()) and, when the
/// person is matched to a 3D Gaze subject (match_gaze_to_pose()), that
/// subject's name, target type and target at the gaze tick within 50 ms,
/// and whether it is mutual gaze. Empty gaze columns: no match, or no gaze
/// tick that close.
void write_people_csv(QTextStream& ts, const Skeleton3DResult& pose, const GazeFusionResult& gaze);

} // namespace mosaic
