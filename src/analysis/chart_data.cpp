#include "analysis/chart_data.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mosaic {

namespace {

// Merged [start, end) intervals of one speaker, sorted.
QVector<QPair<double, double>> speech_of(const QVector<ConversationSpurt>& spurts,
                                         const QString& who) {
    QVector<QPair<double, double>> v;
    for (const auto& s : spurts) {
        if (s.speaker == who && s.endMs > s.startMs) {
            v.append({static_cast<double>(s.startMs), static_cast<double>(s.endMs)});
        }
    }
    std::sort(v.begin(), v.end());
    QVector<QPair<double, double>> merged;
    for (const auto& iv : v) {
        if (!merged.isEmpty() && iv.first <= merged.last().second) {
            merged.last().second = std::max(merged.last().second, iv.second);
        } else {
            merged.append(iv);
        }
    }
    return merged;
}

double clipped_length(const QVector<QPair<double, double>>& v, double lo, double hi) {
    double total = 0.0;
    for (const auto& [a, b] : v) {
        total += std::max(0.0, std::min(b, hi) - std::max(a, lo));
    }
    return total;
}

bool inside(const QVector<QPair<double, double>>& v, double t) {
    return std::any_of(v.begin(), v.end(),
                       [t](const auto& iv) { return t >= iv.first && t < iv.second; });
}

} // namespace

QVector<double> bin_counts(const QVector<double>& values, const QVector<double>& edges) {
    QVector<double> counts(edges.size(), 0.0);
    if (edges.isEmpty()) {
        return counts;
    }
    for (double v : values) {
        if (!std::isfinite(v)) {
            continue;
        }
        int b = 0;
        while (b + 1 < edges.size() && v >= edges[b + 1]) {
            ++b;
        }
        counts[b] += 1.0;
    }
    return counts;
}

QStringList bin_labels(const QVector<double>& edges) {
    QStringList out;
    for (int b = 0; b < edges.size(); ++b) {
        out << (b + 1 < edges.size()
                    ? QString("%1-%2").arg(edges[b], 0, 'g', 3).arg(edges[b + 1], 0, 'g', 3)
                    : QString("%1+").arg(edges[b], 0, 'g', 3));
    }
    return out;
}

SpeakingShares speaking_shares(const QVector<ConversationSpurt>& spurts, double startMs,
                               double endMs) {
    SpeakingShares s;
    if (endMs <= startMs) {
        return s;
    }
    const auto subject = speech_of(spurts, "subject");
    const auto other   = speech_of(spurts, "other");
    double both        = 0.0;
    for (const auto& [a, b] : subject) {
        both += clipped_length(other, std::max(a, startMs), std::min(b, endMs));
    }
    const double subj = clipped_length(subject, startMs, endMs);
    const double oth  = clipped_length(other, startMs, endMs);
    s.bothS           = both / 1000.0;
    s.subjectOnlyS    = (subj - both) / 1000.0;
    s.otherOnlyS      = (oth - both) / 1000.0;
    s.silenceS        = std::max(0.0, (endMs - startMs) - (subj + oth - both)) / 1000.0;
    return s;
}

HrByState hr_by_state(const QVector<RppgInterval>& intervals,
                      const QVector<ConversationSpurt>& spurts) {
    const auto subject = speech_of(spurts, "subject");
    const auto other   = speech_of(spurts, "other");
    QVector<double> speaking, listening, silence;
    for (const auto& iv : intervals) {
        if (!iv.nn || iv.ibiMs <= 0.0) {
            continue;
        }
        const double t     = static_cast<double>(iv.tMs);
        const bool speaks  = inside(subject, t);
        const bool listens = inside(other, t);
        if (speaks) {
            speaking << iv.ibiMs;
        }
        if (listens) {
            listening << iv.ibiMs;
        }
        if (!speaks && !listens) {
            silence << iv.ibiMs;
        }
    }
    auto bpm = [](const QVector<double>& ibis) -> std::optional<double> {
        if (ibis.size() < kMinBeatsPerState) {
            return std::nullopt;
        }
        double sum = 0.0;
        for (double v : ibis) {
            sum += v;
        }
        return 60000.0 / (sum / ibis.size());
    };
    HrByState h;
    h.speakingBpm    = bpm(speaking);
    h.listeningBpm   = bpm(listening);
    h.silenceBpm     = bpm(silence);
    h.speakingBeats  = static_cast<int>(speaking.size());
    h.listeningBeats = static_cast<int>(listening.size());
    h.silenceBeats   = static_cast<int>(silence.size());
    return h;
}

Poincare poincare(const QVector<RppgInterval>& intervals) {
    // Intervals come one per pair of adjacent beats, so two neighbours in
    // the list are consecutive; both must be clean.
    Poincare p;
    QVector<double> diffs, sums;
    for (int i = 0; i + 1 < intervals.size(); ++i) {
        const auto& a = intervals[i];
        const auto& b = intervals[i + 1];
        if (!a.nn || !b.nn) {
            continue;
        }
        p.points << QPointF(a.ibiMs, b.ibiMs);
        diffs << (b.ibiMs - a.ibiMs);
        sums << (a.ibiMs + b.ibiMs);
    }
    if (p.points.size() < 3) {
        return p;
    }
    auto var = [](const QVector<double>& v) {
        double m = 0.0;
        for (double x : v) {
            m += x;
        }
        m /= v.size();
        double s = 0.0;
        for (double x : v) {
            s += (x - m) * (x - m);
        }
        return s / (v.size() - 1);
    };
    p.sd1Ms = std::sqrt(var(diffs) / 2.0);
    p.sd2Ms = std::sqrt(var(sums) / 2.0);
    return p;
}

QVector<QPointF> blink_rate(const QVector<FaceDynamicsEvent>& events,
                            const QVector<FaceDynamicsFrame>& frames, double windowMs) {
    QVector<QPointF> out;
    if (frames.size() < 2 || windowMs <= 0.0) {
        return out;
    }
    const double startMs = static_cast<double>(frames.first().timestampMs);
    const double endMs   = static_cast<double>(frames.last().timestampMs);
    if (endMs <= startMs) {
        return out;
    }
    // Equal windows of about windowMs, so no short last window swings wildly.
    const int n       = std::max(1, static_cast<int>(std::lround((endMs - startMs) / windowMs)));
    const double step = (endMs - startMs) / n;
    QVector<double> faceMs(n, 0.0);
    QVector<int> blinks(n, 0);
    auto window_of = [&](double t) {
        return std::clamp(static_cast<int>((t - startMs) / step), 0, n - 1);
    };
    // Each frame with a face stands for the time until the next frame.
    for (int i = 0; i + 1 < frames.size(); ++i) {
        if (frames[i].faceDetected) {
            const double t = static_cast<double>(frames[i].timestampMs);
            faceMs[window_of(t)] += static_cast<double>(frames[i + 1].timestampMs) - t;
        }
    }
    for (const auto& e : events) {
        const double t = static_cast<double>(e.peakMs);
        if (e.kind == "blink" && t >= startMs && t <= endMs) {
            ++blinks[window_of(t)];
        }
    }
    for (int w = 0; w < n; ++w) {
        out << QPointF(startMs + w * step, faceMs[w] >= kMinFaceForRateMs
                                               ? blinks[w] * 60000.0 / faceMs[w]
                                               : std::numeric_limits<double>::quiet_NaN());
    }
    return out;
}

} // namespace mosaic
