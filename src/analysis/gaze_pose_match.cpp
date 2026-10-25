#include "analysis/gaze_pose_match.hpp"

#include <QMap>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <vector>

#include "analysis/dyadic_kinematics.hpp"
#include "analysis/pose3d_people.hpp"

namespace mosaic {

namespace {

constexpr int64_t kSameTimeNs = 50000000; // a pose frame this close to a gaze tick
constexpr int kMinSharedTicks = 5;

double median(std::vector<double> v) {
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

} // namespace

QVector<GazePoseMatch> match_gaze_to_pose(const GazeFusionResult& gaze,
                                          const Skeleton3DResult& pose, double withinMm,
                                          double share) {
    QVector<GazePoseMatch> out;
    if (!gaze.is_valid()) {
        return out;
    }
    const QStringList& subjects = gaze.subject_ids();
    for (const auto& id : subjects) {
        out.push_back({id});
    }
    if (!pose.is_valid() || pose.frames().isEmpty()) {
        return out;
    }

    QVector<int> face;
    for (const char* n : {"nose", "left_eye", "right_eye", "left_ear", "right_ear"}) {
        if (const int i = static_cast<int>(pose.keypoint_names().indexOf(n)); i >= 0) {
            face << i;
        }
    }
    const auto head = [&](const Skeleton3DPerson& p) -> std::optional<Skeleton3DVec3> {
        Skeleton3DVec3 sum{0, 0, 0};
        int n = 0;
        for (const int i : face) {
            if (i < p.keypoints.size() && p.keypoints[i].valid) {
                for (int c = 0; c < 3; ++c) {
                    sum[c] += p.keypoints[i].positionRoom[c];
                }
                ++n;
            }
        }
        if (n == 0) {
            return std::nullopt;
        }
        return Skeleton3DVec3{sum[0] / n, sum[1] / n, sum[2] / n};
    };

    // The distances between each subject's and each person's heads, tick
    // by tick.
    QMap<QPair<QString, int>, std::vector<double>> distances;
    for (const auto& f : gaze.frames()) {
        const Skeleton3DFrame* pf = pose.nearest_frame(f.timestampNs);
        if (!pf || std::llabs(pf->timestampNs - f.timestampNs) > kSameTimeNs) {
            continue;
        }
        for (const auto& s : f.subjects) {
            for (const auto& p : pf->people) {
                if (const auto h = head(p)) {
                    const double dx = s.origin[0] - (*h)[0], dy = s.origin[1] - (*h)[1],
                                 dz = s.origin[2] - (*h)[2];
                    distances[{s.id, p.trackId}].push_back(std::sqrt(dx * dx + dy * dy + dz * dz));
                }
            }
        }
    }

    // Candidates, closest first; each subject and person once.
    std::vector<GazePoseMatch> candidates;
    for (auto it = distances.cbegin(); it != distances.cend(); ++it) {
        const auto& d = it.value();
        if (static_cast<int>(d.size()) < kMinSharedTicks) {
            continue;
        }
        const int agreeing = static_cast<int>(
            std::count_if(d.begin(), d.end(), [&](double x) { return x <= withinMm; }));
        const double within = static_cast<double>(agreeing) / static_cast<double>(d.size());
        const double med    = median(d);
        if (within >= share && med <= withinMm) {
            candidates.push_back({it.key().first, it.key().second, med, within,
                                  static_cast<int>(d.size()), agreeing});
        }
    }
    // Most ticks together first, then the closest.
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.agreeingTicks != b.agreeingTicks ? a.agreeingTicks > b.agreeingTicks
                                                  : a.medianMm < b.medianMm;
    });
    QSet<QString> takenSubjects;
    QSet<int> takenPeople;
    for (const auto& c : candidates) {
        if (takenSubjects.contains(c.subject) || takenPeople.contains(c.person)) {
            continue;
        }
        takenSubjects.insert(c.subject);
        takenPeople.insert(c.person);
        for (auto& m : out) {
            if (m.subject == c.subject) {
                m = c;
            }
        }
    }
    return out;
}

void write_people_csv(QTextStream& ts, const Skeleton3DResult& pose, const GazeFusionResult& gaze) {
    if (!pose.is_valid() || pose.frames().isEmpty()) {
        return;
    }
    QMap<int, QString> subjectOf; // person -> gaze subject id
    QStringList matched;
    // Names come from users (gaze_targets.json): one line in the comment,
    // quoted in the table when they hold a comma, a quote or a line break.
    const auto oneLine = [](QString v) { return v.replace('\r', ' ').replace('\n', ' '); };
    const auto text    = [](const QString& v) {
        if (!v.contains(',') && !v.contains('"') && !v.contains('\n') && !v.contains('\r')) {
            return v;
        }
        return QString("\"%1\"").arg(QString(v).replace("\"", "\"\""));
    };
    for (const auto& m : match_gaze_to_pose(gaze, pose)) {
        if (m.person >= 0) {
            subjectOf[m.person] = m.subject;
            matched << QString("%1 = Person %2")
                           .arg(oneLine(gaze.subject_name(m.subject)))
                           .arg(m.person);
        }
    }
    QMap<int, PersonKinematicsSeries> people;
    for (const int id : pose.seconds_by_person().keys()) {
        people[id] = compute_person_kinematics(pose, id);
    }

    ts << "# one row per tick and person in 3D; the columns as person_<n>.csv's; gaze_*: "
          "the matched 3D Gaze person's"
       << (gaze.is_valid()
               ? (matched.isEmpty() ? QString(" (none matched)") : " (" + matched.join("; ") + ")")
               : QString(" (no 3D Gaze result)"))
       << "\n";
    ts << "tick,timestamp_ns,person," << person_csv_columns()
       << ",gaze_subject,gaze_target_type,gaze_target,gaze_mutual\n";
    const auto& frames = pose.frames();
    for (int k = 0; k < frames.size(); ++k) {
        // The gaze tick for this frame: within 50 ms.
        const GazeFusionFrame* g =
            gaze.is_valid() ? gaze.nearest_frame(frames[k].timestampNs) : nullptr;
        if (g && std::llabs(g->timestampNs - frames[k].timestampNs) > kSameTimeNs) {
            g = nullptr;
        }
        for (const auto& person : frames[k].people) {
            const auto it = people.constFind(person.trackId);
            if (it == people.cend()) {
                continue;
            }
            QString gazeCols = ",,,";
            if (const QString sub = subjectOf.value(person.trackId); !sub.isEmpty()) {
                gazeCols = text(gaze.subject_name(sub)) + ",,,";
                if (const GazeSubjectSample* gs = g ? g->subject(sub) : nullptr) {
                    gazeCols = text(gs->name) + "," + gs->targetType + "," + text(gs->targetLabel) +
                               "," + (gs->mutual ? "true" : "false");
                }
            }
            ts << frames[k].tick << "," << frames[k].timestampNs << "," << person.trackId << ","
               << person_csv_values(it.value().samples[k]) << "," << gazeCols << "\n";
        }
    }
}

} // namespace mosaic
