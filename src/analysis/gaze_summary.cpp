#include "analysis/gaze_summary.hpp"

#include <QMap>
#include <algorithm>
#include <vector>

namespace mosaic {

namespace {

// What a sample counts as: its target (by type and id or name), nothing,
// or no gaze.
GazeTarget target_of(const GazeSubjectSample& s) {
    if (!s.hasDirection) {
        return {kGazeNoGaze, kGazeNoGaze};
    }
    if (s.targetType.isEmpty() || s.targetType == QLatin1String("none") ||
        s.targetLabel.isEmpty()) {
        return {kGazeNothing, kGazeNothing};
    }
    if (s.targetType == QLatin1String("subject")) {
        return {"subject/" + s.targetSubject, s.targetLabel};
    }
    if (s.targetType == QLatin1String("region")) {
        return {"region/" + s.targetLabel, s.targetLabel};
    }
    return {s.targetType, s.targetLabel}; // the plane
}

// Adds the tick at `ns` on `target` to `spans`: onto the last stretch when
// it is the same target and the tick follows it, else a new one.
void extend(QVector<GazeSpan>& spans, const GazeTarget& target, int64_t ns, int64_t stepNs) {
    if (!spans.isEmpty() && spans.last().target.key == target.key &&
        ns - (spans.last().endNs - stepNs) <= stepNs * 3 / 2) {
        spans.last().endNs = ns + stepNs;
        return;
    }
    spans.push_back({target, ns, ns + stepNs});
}

} // namespace

QVector<GazeLanding> gaze_landings(const GazeFusionResult& result, const QString& id,
                                   const GazeFloorFrame& floor) {
    const auto dot = [](const Vec3& a, const Vec3& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const Vec3& u = floor.up;
    const Vec3& f = floor.forward;
    const Vec3 right{f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2],
                     f[0] * u[1] - f[1] * u[0]};
    QVector<GazeLanding> out;
    for (const auto& frame : result.frames()) {
        const GazeSubjectSample* s = frame.subject(id);
        if (!s || !s->hasPoint) {
            continue;
        }
        GazeLanding g;
        g.timestampNs = frame.timestampNs;
        g.across      = dot(s->point, right) / 1000.0;
        g.forward     = dot(s->point, f) / 1000.0;
        g.height      = (dot(s->point, u) - floor.floorMm) / 1000.0;
        g.headAcross  = dot(s->origin, right) / 1000.0;
        g.headForward = dot(s->origin, f) / 1000.0;
        g.onTarget    = target_of(*s).is_target();
        out.push_back(g);
    }
    return out;
}

GazeSummary summarize_gaze(const GazeFusionResult& result) {
    GazeSummary out;
    const auto& frames = result.frames();
    if (!result.is_valid() || frames.isEmpty()) {
        return out;
    }

    // One analysed tick's step: the median gap, else from the frame rate.
    std::vector<int64_t> gaps;
    for (int k = 1; k < frames.size(); ++k) {
        if (const int64_t g = frames[k].timestampNs - frames[k - 1].timestampNs; g > 0) {
            gaps.push_back(g);
        }
    }
    int64_t stepNs = 0;
    if (!gaps.empty()) {
        std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
        stepNs = gaps[gaps.size() / 2];
    } else {
        stepNs = static_cast<int64_t>(1e9 * std::max(1, result.analysed_every()) /
                                      std::max(1.0, result.master_fps()));
    }
    out.stepS = static_cast<double>(stepNs) / 1e9;

    const QStringList& ids = result.subject_ids();
    for (const auto& id : ids) {
        GazeSubjectSummary s;
        s.id   = id;
        s.name = result.subject_name(id);
        QMap<QString, GazeTargetTime> byKey;
        int seen = 0;
        for (const auto& f : frames) {
            if (const GazeSubjectSample* sample = f.subject(id)) {
                const GazeTarget target = target_of(*sample);
                auto& t                 = byKey[target.key];
                t.target                = target;
                t.seconds += out.stepS;
                ++seen;
                extend(s.spans, target, f.timestampNs, stepNs);
            }
        }
        s.secondsSeen     = seen * out.stepS;
        s.secondsByTarget = byKey.values().toVector();
        std::stable_sort(s.secondsByTarget.begin(), s.secondsByTarget.end(),
                         [](const auto& x, const auto& y) { return x.seconds > y.seconds; });
        out.subjects.push_back(std::move(s));
    }

    // Mutual gaze, pair by pair: a tick counts when a looks at b's face and
    // the result marks it mutual (b looking back).
    for (int i = 0; i < ids.size(); ++i) {
        for (int j = i + 1; j < ids.size(); ++j) {
            GazePairSummary p;
            p.a             = ids[i];
            p.b             = ids[j];
            p.nameA         = result.subject_name(p.a);
            p.nameB         = result.subject_name(p.b);
            int mutualTicks = 0;
            for (const auto& f : frames) {
                const GazeSubjectSample* a = f.subject(p.a);
                if (a && a->mutual && a->targetType == QLatin1String("subject") &&
                    a->targetSubject == p.b) {
                    ++mutualTicks;
                    extend(p.spans, {}, f.timestampNs, stepNs);
                }
            }
            p.seconds = mutualTicks * out.stepS;
            for (const auto& span : p.spans) {
                p.longestS =
                    std::max(p.longestS, static_cast<double>(span.endNs - span.startNs) / 1e9);
            }
            out.pairs.push_back(std::move(p));
        }
    }
    return out;
}

} // namespace mosaic
