#include "analysis/session_overview.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "analysis/conversation_result.hpp"
#include "analysis/eye_contact_result.hpp"
#include "analysis/face_dynamics_result.hpp"
#include "analysis/rppg_result.hpp"

namespace mosaic {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool usable(const ConversationResult* r) { return r != nullptr && r->is_valid(); }
bool usable(const EyeContactResult* r) {
    return r != nullptr && r->is_valid() && !r->frames().isEmpty();
}
bool usable(const FaceDynamicsResult* r) {
    return r != nullptr && r->is_valid() && !r->frames().isEmpty();
}
bool usable(const RppgResult* r) {
    return r != nullptr && r->is_valid() && !r->windows().isEmpty();
}

// A high percentile, so one jerk of the head does not flatten the lane.
double percentile(QVector<double> v, double q) {
    v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return !std::isfinite(x); }),
            v.end());
    if (v.isEmpty()) {
        return 0.0;
    }
    const auto k = static_cast<qsizetype>(q * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

} // namespace

SessionOverview build_session_overview(const OverviewSources& src) {
    // Time origin: the earliest first frame any source saw (all are the same
    // camera's elapsed clock), and the end: the latest last frame.
    double t0 = std::numeric_limits<double>::max();
    double t1 = std::numeric_limits<double>::lowest();
    auto span = [&](double a, double b) {
        t0 = std::min(t0, a);
        t1 = std::max(t1, b);
    };
    if (usable(src.conversation)) {
        span(src.conversation->video_start_ms(), src.conversation->video_start_ms());
        for (const auto& s : src.conversation->spurts()) {
            t1 = std::max(t1, static_cast<double>(s.endMs));
        }
        // Its signals run over the whole recording, speech or none.
        const auto n = std::max(src.conversation->audio_db().size(),
                                src.conversation->mouth_activity().size());
        if (n > 0) {
            t1 = std::max(t1, src.conversation->signal_start_ms() +
                                  static_cast<double>(n - 1) * src.conversation->signal_step_ms());
        }
    }
    if (usable(src.eyeContact)) {
        span(static_cast<double>(src.eyeContact->frames().first().timestampMs),
             static_cast<double>(src.eyeContact->frames().last().timestampMs));
    }
    if (usable(src.faceDynamics)) {
        span(static_cast<double>(src.faceDynamics->frames().first().timestampMs),
             static_cast<double>(src.faceDynamics->frames().last().timestampMs));
    }
    if (usable(src.rppg)) {
        if (!src.rppg->frames().isEmpty()) {
            span(static_cast<double>(src.rppg->frames().first().timestampMs),
                 static_cast<double>(src.rppg->frames().last().timestampMs));
        } else {
            span(static_cast<double>(src.rppg->windows().first().startMs),
                 static_cast<double>(src.rppg->windows().last().endMs));
        }
    }
    SessionOverview out;
    const bool any = t0 <= t1;
    if (!any) {
        t0 = t1 = 0.0;
    }
    out.durationMs = std::max(0.0, t1 - t0);

    // ── Who speaks ──
    OverviewLane speaking{"speaking", "Who speaks", OverviewLane::Kind::Spans};
    if (usable(src.conversation)) {
        for (const auto& s : src.conversation->spurts()) {
            if (s.speaker != "subject" && s.speaker != "other") {
                continue;
            }
            speaking.spans.append({static_cast<double>(s.startMs) - t0,
                                   static_cast<double>(s.endMs) - t0,
                                   s.speaker == "subject" ? 0 : 1});
        }
    } else {
        speaking.note = "Conversation Timing not run";
    }
    out.lanes.append(speaking);

    // ── Eye contact: runs of looking at the partner, and of looking away ──
    OverviewLane eye{"eye_contact", "Eye contact", OverviewLane::Kind::Spans};
    if (usable(src.eyeContact)) {
        const auto& fr = src.eyeContact->frames();
        int runState   = -1;
        double runFrom = 0.0;
        for (int i = 0; i <= fr.size(); ++i) {
            const int state  = i < fr.size() ? fr[i].contact : -2; // -2: past the end
            const double now = i < fr.size() ? static_cast<double>(fr[i].timestampMs) - t0
                                             : static_cast<double>(fr.last().timestampMs) - t0;
            if (state != runState) {
                if (runState == 0 || runState == 1) {
                    eye.spans.append({runFrom, now, runState == 1 ? 0 : 1});
                }
                runState = state;
                runFrom  = now;
            }
        }
    } else {
        eye.note = "Eye Contact not run";
    }
    out.lanes.append(eye);

    // ── Smile and head movement (Face Dynamics) ──
    OverviewLane smile{"smile", "Smile", OverviewLane::Kind::Line};
    OverviewLane head{"head", "Head movement", OverviewLane::Kind::Line};
    head.unit = "deg/s";
    if (usable(src.faceDynamics)) {
        QVector<double> speeds;
        for (const auto& f : src.faceDynamics->frames()) {
            const double x = static_cast<double>(f.timestampMs) - t0;
            smile.points.append(QPointF(x, f.faceDetected ? f.smile : kNaN));
            head.points.append(QPointF(x, f.faceDetected ? f.headSpeed : kNaN));
            if (f.faceDetected) {
                speeds.append(f.headSpeed);
            }
        }
        smile.minY = 0.0;
        smile.maxY = 1.0;
        head.minY  = 0.0;
        head.maxY  = std::max(10.0, percentile(speeds, 0.99));
    } else {
        smile.note = "Face Dynamics not run";
        head.note  = "Face Dynamics not run";
    }
    out.lanes.append(smile);
    out.lanes.append(head);

    // ── Heart rate: each window's rate over the stretch it stands for ──
    OverviewLane hr{"heart_rate", "Heart rate", OverviewLane::Kind::Line};
    hr.unit = "bpm";
    if (usable(src.rppg)) {
        const auto& w = src.rppg->windows();
        double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest();
        auto centre = [&](int i) { return (w[i].startMs + w[i].endMs) / 2.0 - t0; };
        for (int i = 0; i < w.size(); ++i) {
            if (std::isnan(w[i].bpm)) {
                // A gap, never filled in, from where this window's stretch
                // starts (so reading the line there finds no value).
                const double from = i == 0 ? static_cast<double>(w[i].startMs) - t0
                                           : (centre(i) + centre(i - 1)) / 2.0;
                hr.points.append(QPointF(std::clamp(from, 0.0, out.durationMs), kNaN));
                continue;
            }
            const double from =
                i == 0 ? static_cast<double>(w[i].startMs) - t0 : (centre(i) + centre(i - 1)) / 2.0;
            const double to =
                i + 1 == w.size() ? out.durationMs : (centre(i) + centre(i + 1)) / 2.0;
            const double a = std::clamp(from, 0.0, out.durationMs);
            const double b = std::clamp(to, 0.0, out.durationMs);
            if (b <= a) {
                continue;
            }
            hr.points.append(QPointF(a, w[i].bpm));
            hr.points.append(QPointF(b, w[i].bpm));
            lo = std::min(lo, w[i].bpm);
            hi = std::max(hi, w[i].bpm);
        }
        if (lo <= hi) {
            // Close to the data (at least 10 bpm tall), so a change of a
            // few bpm shows.
            const double mid  = (lo + hi) / 2.0;
            const double half = std::max(5.0, (hi - lo) / 2.0 + 2.0);
            hr.minY           = std::floor(mid - half);
            hr.maxY           = std::ceil(mid + half);
        } else {
            hr.note = "Heart rate: no window had a reliable estimate";
        }
    } else {
        hr.note = "Remote Heart Rate not run";
    }
    out.lanes.append(hr);
    return out;
}

} // namespace mosaic
