#pragma once
#include <QJsonObject>
#include <QString>

#include "analysis/dyadic_kinematics.hpp"
#include "analysis/skeleton3d_result.hpp"

namespace mosaic {

/// A posture as the exports write it: "sitting", "standing", "walking";
/// empty where unknown.
[[nodiscard]] QString posture_name(Posture p);

/// The per-person columns of the 3D Pose exports, after tick and
/// timestamp_ns: speed, heights, cameras, joint angles, movement, posture.
[[nodiscard]] QString person_csv_columns();
/// One sample's values for person_csv_columns(), comma separated, empty
/// where unknown.
[[nodiscard]] QString person_csv_values(const PersonSample& s);

/// Every person's summary over a 3D Pose result, as
/// <session>/pose3d_people.json ("mosaic-pose3d-people-v1") holds it for the
/// session report: per person, the time in 3D and in each posture, the top
/// speed, the usual head height and joint angles, and how much they moved
/// (usual and high: median and 95th percentile). From
/// compute_person_kinematics(), the code the Charts tab draws from, so the
/// report and the charts agree.
[[nodiscard]] QJsonObject pose3d_people_summary(const Skeleton3DResult& result);

/// Writes pose3d_people_summary() to <sessionDir>/pose3d_people.json when
/// it is missing or older than <sessionDir>/skeleton3d.json; `result` is
/// that file's content. True when the file is now up to date.
bool write_pose3d_people_if_stale(const QString& sessionDir, const Skeleton3DResult& result);

/// Whether <sessionDir>/pose3d_people.json is missing or older than its
/// skeleton3d.json (false when there is no skeleton3d.json).
[[nodiscard]] bool pose3d_people_stale(const QString& sessionDir);

} // namespace mosaic
