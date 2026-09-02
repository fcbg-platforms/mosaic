#include "session/session_health.hpp"

#include <algorithm>

namespace mosaic {

RmsQuality camera_health_quality_for(const CameraHealthInput& raw,
                                     std::optional<int64_t> missedTriggerFrames) {
    // Checked FIRST, both of them: a camera that never opened, or opened and
    // captured nothing, has no drop/incomplete/sync data to be "bad" in, and
    // its achievableFps stays at the "not measured" sentinel (-1) — so every
    // other check below would silently grade it Excellent. That inversion is
    // exactly what made a disconnected camera outrank healthy ones. Kept as
    // two separate conditions because the dialog renders them differently
    // ("not opened" vs. real zeros).
    if (!raw.participated) {
        return RmsQuality::Poor;
    }
    if (raw.framesGrabbed <= 0) {
        return RmsQuality::Poor;
    }
    if (raw.framesDropped > 0 || raw.incompleteFrames > 0) {
        return RmsQuality::Poor;
    }
    if (raw.syncCoveragePct.has_value() && *raw.syncCoveragePct < 80.0) {
        return RmsQuality::Poor;
    }
    if (missedTriggerFrames.has_value() && *missedTriggerFrames > 5) {
        return RmsQuality::Poor;
    }
    if (raw.syncCoveragePct.has_value() && *raw.syncCoveragePct < 95.0) {
        return RmsQuality::Acceptable;
    }
    if (raw.achievableFps > 0.0 && raw.configuredFps > 0.0 &&
        raw.achievableFps < raw.configuredFps * 0.9) {
        return RmsQuality::Acceptable;
    }
    if (raw.achievableFps > 0.0 && raw.configuredFps > 0.0 &&
        raw.achievableFps < raw.configuredFps * 0.98) {
        return RmsQuality::Good;
    }
    return RmsQuality::Excellent;
}

namespace {

// Ordinal rank for "worst wins" aggregation — higher means worse.
int quality_rank(RmsQuality q) {
    switch (q) {
        case RmsQuality::Excellent:
            return 0;
        case RmsQuality::Good:
            return 1;
        case RmsQuality::Acceptable:
            return 2;
        case RmsQuality::Poor:
            return 3;
    }
    return 3;
}

QString quality_label(RmsQuality q) {
    switch (q) {
        case RmsQuality::Excellent:
            return "Excellent";
        case RmsQuality::Good:
            return "Good";
        case RmsQuality::Acceptable:
            return "Acceptable";
        case RmsQuality::Poor:
            return "Poor";
    }
    return "Poor";
}

} // namespace

SessionHealthReport build_session_health_report(const QString& sessionPath,
                                                const QString& sessionName, int64_t durationMs,
                                                const QVector<CameraHealthInput>& cameras) {
    SessionHealthReport report;
    report.sessionPath = sessionPath;
    report.sessionName = sessionName;
    report.durationMs  = durationMs;
    report.cameras.reserve(cameras.size());

    int worstRank                       = 0;
    const CameraHealthEntry* worstEntry = nullptr;
    int cleanCount                      = 0;

    for (const auto& raw : cameras) {
        CameraHealthEntry entry;
        entry.raw = raw;
        if (raw.actionTicksFired.has_value()) {
            entry.missedTriggerFrames =
                std::max<int64_t>(0, *raw.actionTicksFired - raw.framesGrabbed);
        }
        entry.quality = camera_health_quality_for(raw, entry.missedTriggerFrames);
        report.cameras.push_back(entry);

        const int rank = quality_rank(entry.quality);
        if (worstEntry == nullptr || rank > worstRank) {
            worstRank  = rank;
            worstEntry = &report.cameras.back();
        }
        if (entry.quality == RmsQuality::Excellent || entry.quality == RmsQuality::Good) {
            ++cleanCount;
        }
    }

    if (report.cameras.isEmpty()) {
        report.overallQuality = RmsQuality::Excellent;
        report.headline       = "No cameras recorded.";
        return report;
    }

    report.overallQuality = worstEntry->quality;
    if (worstRank == 0) {
        report.headline = QString("%1/%2 cameras clean").arg(cleanCount).arg(report.cameras.size());
    } else {
        // 1-based to match the producer's own label and CameraCardW's headers.
        // (The on-disk artifacts are 0-based — video_0.mp4 is Camera 1 — which
        // the dialog's tooltip spells out.)
        const QString name = worstEntry->raw.name.isEmpty()
                                 ? QString("Camera %1").arg(worstEntry->raw.index + 1)
                                 : worstEntry->raw.name;
        report.headline    = QString("%1: %2").arg(name, quality_label(worstEntry->quality));
    }
    return report;
}

} // namespace mosaic
