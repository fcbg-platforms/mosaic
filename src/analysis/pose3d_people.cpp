#include "analysis/pose3d_people.hpp"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <cmath>

namespace mosaic {

namespace {

QJsonValue num(double v, int decimals) {
    if (std::isnan(v)) {
        return QJsonValue(); // null: unknown
    }
    const double scale = std::pow(10.0, decimals);
    return std::round(v * scale) / scale;
}

QJsonArray sides(const LeftRight& lr, int decimals) {
    return {num(lr.left, decimals), num(lr.right, decimals)};
}

QJsonObject spread(const QVector<PersonSample>& samples,
                   const std::function<double(const PersonSample&)>& value, double scale,
                   int decimals) {
    const Spread s = spread_of(samples, value);
    return {{"median", num(s.median * scale, decimals)}, {"p95", num(s.p95 * scale, decimals)}};
}

} // namespace

QString posture_name(Posture p) {
    switch (p) {
        case Posture::Sitting:
            return "sitting";
        case Posture::Standing:
            return "standing";
        case Posture::Walking:
            return "walking";
        default:
            return QString();
    }
}

QString person_csv_columns() {
    return "speed_mm_s,head_height_mm,hip_height_mm,cameras,"
           "knee_flexion_left_deg,knee_flexion_right_deg,"
           "hip_flexion_left_deg,hip_flexion_right_deg,"
           "elbow_flexion_left_deg,elbow_flexion_right_deg,"
           "arm_raise_left_deg,arm_raise_right_deg,"
           "wrist_speed_left_mm_s,wrist_speed_right_mm_s,"
           "knee_flexion_rate_left_deg_s,knee_flexion_rate_right_deg_s,"
           "elbow_flexion_rate_left_deg_s,elbow_flexion_rate_right_deg_s,"
           "motion_mm_s,head_speed_mm_s,head_turn_deg_s,posture";
}

QString person_csv_values(const PersonSample& s) {
    const auto n = [](double v) { return std::isnan(v) ? QString() : QString::number(v, 'f', 1); };
    const auto both = [&](const LeftRight& a) { return n(a.left) + "," + n(a.right); };
    return QStringList{n(s.speedMmPerS),
                       n(s.headHeightMm),
                       n(s.hipHeightMm),
                       QString::number(s.cameras),
                       both(s.kneeFlexionDeg),
                       both(s.hipFlexionDeg),
                       both(s.elbowFlexionDeg),
                       both(s.armRaiseDeg),
                       both(s.wristSpeedMmPerS),
                       both(s.kneeFlexionRateDegPerS),
                       both(s.elbowFlexionRateDegPerS),
                       n(s.motionMmPerS),
                       n(s.headSpeedMmPerS),
                       n(s.headTurnDegPerS),
                       posture_name(s.posture)}
        .join(",");
}

QJsonObject pose3d_people_summary(const Skeleton3DResult& result) {
    QJsonArray people;
    const auto seconds = result.seconds_by_person();
    const auto lasting = result.lasting_people();
    for (auto it = seconds.cbegin(); it != seconds.cend(); ++it) {
        const int id = it.key();
        // Under a second in 3D (a passing false detection, a split track's
        // end): hidden in MOSAIC and the report, so not worth a pass over
        // the whole recording each.
        if (!lasting.contains(id)) {
            people.append(QJsonObject{{"person", id},
                                      {"label", QString("Person %1").arg(id)},
                                      {"brief", true},
                                      {"seconds_in_3d", num(it.value(), 1)}});
            continue;
        }
        const auto k       = compute_person_kinematics(result, id);
        const auto& st     = k.stats;
        const auto& s      = k.samples;
        const auto posture = [&](Posture p) {
            return num(st.secondsByPosture[static_cast<int>(p)], 1);
        };
        people.append(QJsonObject{
            {"person", id},
            {"label", QString("Person %1").arg(id)},
            {"brief", false},
            {"seconds_in_3d", num(st.secondsSeen, 1)},
            {"seconds_by_posture", QJsonObject{{"sitting", posture(Posture::Sitting)},
                                               {"standing", posture(Posture::Standing)},
                                               {"walking", posture(Posture::Walking)},
                                               {"unknown", posture(Posture::Unknown)}}},
            {"top_speed_m_s", num(st.topSpeedMmPerS / 1000.0, 2)},
            {"median_head_height_m", num(st.medianHeadHeightMm / 1000.0, 2)},
            {"median_angles_deg", QJsonObject{{"knee_flexion", sides(st.medianKneeFlexionDeg, 0)},
                                              {"hip_flexion", sides(st.medianHipFlexionDeg, 0)},
                                              {"elbow_flexion", sides(st.medianElbowFlexionDeg, 0)},
                                              {"arm_raise", sides(st.medianArmRaiseDeg, 0)}}},
            {"movement",
             QJsonObject{
                 {"motion_m_s",
                  spread(
                      s, [](const PersonSample& x) { return x.motionMmPerS; }, 1e-3, 2)},
                 {"wrist_left_m_s",
                  spread(
                      s, [](const PersonSample& x) { return x.wristSpeedMmPerS.left; }, 1e-3, 2)},
                 {"wrist_right_m_s",
                  spread(
                      s, [](const PersonSample& x) { return x.wristSpeedMmPerS.right; }, 1e-3, 2)},
                 {"head_turn_deg_s",
                  spread(
                      s, [](const PersonSample& x) { return x.headTurnDegPerS; }, 1.0, 0)}}},
        });
    }
    return {{"schema", "mosaic-pose3d-people-v1"},
            {"source", "skeleton3d.json"},
            {"note",
             "Written by MOSAIC from skeleton3d.json, with the 3D Pose charts' code "
             "(docs/math/pose3d_reconstruction.rst). Movement: median and 95th "
             "percentile. Angles: flexion in degrees, [left, right]."},
            {"people", people}};
}

bool pose3d_people_stale(const QString& sessionDir) {
    const QFileInfo skeleton(sessionDir + "/skeleton3d.json");
    const QFileInfo people(sessionDir + "/pose3d_people.json");
    return skeleton.exists() &&
           (!people.exists() || people.lastModified() < skeleton.lastModified());
}

bool write_pose3d_people_if_stale(const QString& sessionDir, const Skeleton3DResult& result) {
    if (!pose3d_people_stale(sessionDir)) {
        return true;
    }
    if (!result.is_valid()) {
        return false;
    }
    // Whole or not at all: a half-written file dated now would pass for up
    // to date and never be rewritten.
    QSaveFile f(sessionDir + "/pose3d_people.json");
    const QByteArray json =
        QJsonDocument(pose3d_people_summary(result)).toJson(QJsonDocument::Indented);
    if (!f.open(QIODevice::WriteOnly) || f.write(json) != json.size() || !f.commit()) {
        return false;
    }
    // A skeleton3d.json dated in the future (copied from a PC whose clock
    // was ahead) would leave this older for good, and rewritten each time
    // the result opens: dated as it is instead.
    const QDateTime skeleton = QFileInfo(sessionDir + "/skeleton3d.json").lastModified();
    if (skeleton > QDateTime::currentDateTime()) {
        QFile written(sessionDir + "/pose3d_people.json");
        if (written.open(QIODevice::ReadWrite)) {
            written.setFileTime(skeleton, QFileDevice::FileModificationTime);
        }
    }
    return true;
}

} // namespace mosaic
