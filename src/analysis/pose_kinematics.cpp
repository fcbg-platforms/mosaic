#include "analysis/pose_kinematics.hpp"
#include <algorithm>
#include <cmath>

namespace mosaic {

namespace {

struct RawSample {
    int64_t timestampNs;
    QPointF position;
};

// Collects (timestamp, position) for every frame where the given subject and
// keypoint exist and are visible enough (is_keypoint_visible(), shared with
// SkeletonOverlayW::paintEvent's paint condition — see
// src/analysis/pose_analysis_result.hpp). A frame that fails any of these
// checks is skipped, not interpolated: velocity computed between the two
// nearest surviving samples then uses their real elapsed time, which
// correctly reports an honest average speed across a detection dropout (of
// any length — there's no max-gap cutoff, since there's no empirical basis
// for picking one; a long gap yields a low-but-real average rather than a
// fabricated spike or a divide-by-assumed-frame-rate error).
QVector<RawSample> collect_valid_samples(const PoseAnalysisResult& result,
                                          int keypointIndex, int subjectIndex) {
    QVector<RawSample> raw;
    for (const auto& frame : result.frames()) {
        if (subjectIndex < 0 || subjectIndex >= frame.subjects.size()) { continue; }
        const auto& subject = frame.subjects[subjectIndex];
        if (keypointIndex < 0 || keypointIndex >= subject.keypoints.size()) { continue; }
        if (!is_keypoint_visible(subject, keypointIndex)) { continue; }
        raw.append({frame.timestampNs, subject.keypoints[keypointIndex]});
    }
    return raw;
}

// Centered moving average over the valid-sample sequence (positions only —
// timestamps are untouched). smoothingWindow <= 1 is a no-op. A centered
// (not trailing) window is correct here because this is offline analysis
// over an already-complete recording, so there's no reason to accept a
// causal filter's phase lag. halfWidth uses integer division, so an even
// smoothingWindow is treated as the next-higher odd width (e.g. 4 -> half-
// width 2 -> an actual 5-wide window; a centered window needs a middle
// sample) — the UI is expected to only offer odd values via its spinbox
// step, but this stays safe (just slightly wider than requested) if called
// with an even one anyway. Windows are clamped at the sequence edges rather
// than padded, i.e. the average simply uses fewer samples near the start/end.
QVector<QPointF> smooth_positions(const QVector<RawSample>& raw, int smoothingWindow) {
    QVector<QPointF> smoothed;
    smoothed.reserve(raw.size());

    if (smoothingWindow <= 1) {
        for (const auto& s : raw) { smoothed.append(s.position); }
        return smoothed;
    }

    const int halfWidth = smoothingWindow / 2;
    const int lastIndex  = static_cast<int>(raw.size()) - 1;
    for (int i = 0; i < raw.size(); ++i) {
        const int lo = std::max(0, i - halfWidth);
        const int hi = std::min(lastIndex, i + halfWidth);
        double sx = 0.0;
        double sy = 0.0;
        for (int j = lo; j <= hi; ++j) {
            sx += raw[j].position.x();
            sy += raw[j].position.y();
        }
        const int n = hi - lo + 1;
        smoothed.append(QPointF(sx / n, sy / n));
    }
    return smoothed;
}

} // namespace

KinematicsSeries compute_kinematics(const PoseAnalysisResult& result,
                                     int keypointIndex, int subjectIndex,
                                     int smoothingWindow) {
    KinematicsSeries out;
    if (!result.is_valid()) { return out; }

    const QVector<RawSample> raw = collect_valid_samples(result, keypointIndex, subjectIndex);
    if (raw.isEmpty()) { return out; }

    const QVector<QPointF> smoothed = smooth_positions(raw, smoothingWindow);

    out.samples.reserve(raw.size());
    for (int i = 0; i < raw.size(); ++i) {
        KinematicSample sample;
        sample.timestampNs = raw[i].timestampNs;
        sample.position     = smoothed[i];
        out.samples.append(sample);
    }

    // Total distance uses PRE-smoothing positions — smoothing exists to
    // stabilize the derivative estimate (speed/acceleration), not to
    // understate how far the tracked point actually moved.
    double totalDistance = 0.0;
    for (int i = 1; i < raw.size(); ++i) {
        const double dx = raw[i].position.x() - raw[i - 1].position.x();
        const double dy = raw[i].position.y() - raw[i - 1].position.y();
        totalDistance += std::hypot(dx, dy);

        const double dt = (raw[i].timestampNs - raw[i - 1].timestampNs) / 1e9;
        if (dt > 0.0) {
            const double sdx = smoothed[i].x() - smoothed[i - 1].x();
            const double sdy = smoothed[i].y() - smoothed[i - 1].y();
            out.samples[i].speedPxPerS = std::hypot(sdx, sdy) / dt;
        }
    }

    // Acceleration uses the same (t[i]-t[i-1]) delta as the speed[i] sample
    // it's paired with, not a wider two-step span — keeps the "which dt
    // divides this derivative" rule identical at both derivative levels.
    for (int i = 2; i < raw.size(); ++i) {
        const double speedPrev = out.samples[i - 1].speedPxPerS;
        const double speedCur  = out.samples[i].speedPxPerS;
        if (std::isnan(speedPrev) || std::isnan(speedCur)) { continue; }

        const double dt = (raw[i].timestampNs - raw[i - 1].timestampNs) / 1e9;
        if (dt > 0.0) {
            out.samples[i].accelPxPerS2 = (speedCur - speedPrev) / dt;
        }
    }

    out.stats.totalDistancePx = totalDistance;
    double maxSpeed  = std::numeric_limits<double>::lowest();
    bool   anySpeed  = false;
    for (const auto& sample : out.samples) {
        if (std::isnan(sample.speedPxPerS)) { continue; }
        anySpeed = true;
        maxSpeed = std::max(maxSpeed, sample.speedPxPerS);
    }
    if (anySpeed) {
        out.stats.maxSpeedPxPerS = maxSpeed;
        // Average speed is total distance over total elapsed time, NOT the
        // unweighted mean of per-interval speed samples — those samples can
        // span wildly different real durations (a run of tight 1-frame
        // intervals next to one interval spanning a multi-second detection
        // gap), so averaging them as if each carried equal weight would
        // over-count fast dense intervals. This is exactly the case this
        // function's whole gap-handling design is meant to report honestly.
        const double totalDurationS =
            (raw.last().timestampNs - raw.first().timestampNs) / 1e9;
        if (totalDurationS > 0.0) {
            out.stats.avgSpeedPxPerS = totalDistance / totalDurationS;
        }
    }

    return out;
}

} // namespace mosaic
