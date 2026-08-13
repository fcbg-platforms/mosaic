#include "analysis/realtime_metrics.hpp"
#include <cmath>

namespace mosaic {

DetectionRateTracker::DetectionRateTracker(int bucketCount, int64_t bucketDurationMs)
    : maxBuckets_(bucketCount), bucketDurationMs_(bucketDurationMs) {}

void DetectionRateTracker::push(bool detected, int64_t timestampMs) {
    const int64_t bucketDuration = bucketDurationMs_ > 0 ? bucketDurationMs_ : 1;
    const int64_t idx = timestampMs / bucketDuration;

    if (!buckets_.empty() && idx < buckets_.back().index) {
        // Out-of-order timestamp (clock jitter) — drop rather than reopen a
        // bucket that's already been rolled past, since the deque only ever
        // grows at the back / shrinks at the front.
        return;
    }

    if (buckets_.empty() || idx != buckets_.back().index) {
        buckets_.push_back(Bucket{idx, 0, 0});
        while (static_cast<int>(buckets_.size()) > maxBuckets_) {
            buckets_.pop_front();
        }
    }

    Bucket& b = buckets_.back();
    b.total += 1;
    if (detected) { b.hits += 1; }
}

std::optional<double> DetectionRateTracker::rate() const {
    if (buckets_.empty()) { return std::nullopt; }
    int hits = 0, total = 0;
    for (const auto& b : buckets_) {
        hits  += b.hits;
        total += b.total;
    }
    if (total == 0) { return std::nullopt; }
    return static_cast<double>(hits) / static_cast<double>(total);
}

QVector<double> DetectionRateTracker::bucket_rates() const {
    QVector<double> out;
    out.reserve(static_cast<int>(buckets_.size()));
    for (const auto& b : buckets_) {
        out.append(b.total > 0 ? static_cast<double>(b.hits) / static_cast<double>(b.total) : 0.0);
    }
    return out;
}

RmsQuality pose_tracking_quality_for(double detectionRate) {
    if (detectionRate >= 0.85) { return RmsQuality::Excellent; }
    if (detectionRate >= 0.60) { return RmsQuality::Good; }
    if (detectionRate >= 0.30) { return RmsQuality::Acceptable; }
    return RmsQuality::Poor;
}

bool gaze_on_target_for(double gazeDx, double gazeDy, double threshold) {
    return std::hypot(gazeDx, gazeDy) <= threshold;
}

RmsQuality rppg_quality_for(double snrDb, double validFrameFraction) {
    if (validFrameFraction < 0.6) { return RmsQuality::Poor; }
    if (snrDb >= 5.0)  { return RmsQuality::Excellent; }
    if (snrDb >= 0.0)  { return RmsQuality::Good; }
    if (snrDb >= -5.0) { return RmsQuality::Acceptable; }
    return RmsQuality::Poor;
}

} // namespace mosaic
