#pragma once
#include <QPointF>
#include <QStringList>
#include <QVector>
#include <optional>

#include "analysis/conversation_result.hpp"
#include "analysis/expression_result.hpp"
#include "analysis/face_dynamics_result.hpp"
#include "analysis/rppg_result.hpp"

namespace mosaic {

// ── What the Analysis tab's distribution and comparison charts show ─────────
//
// The numbers behind those charts, kept out of the UI so they can be tested.
// Times are elapsed ms, the clock of the video's timestamps.

/// Counts of @p values per bin: bin b holds edges[b] <= v < edges[b + 1], and
/// the last bin everything from edges.back() up (so there are edges.size()
/// bins). Values below edges.front() go in the first bin.
[[nodiscard]] QVector<double> bin_counts(const QVector<double>& values,
                                         const QVector<double>& edges);
/// "0-0.5", "0.5-1", ..., "16+": a label per bin_counts() bin.
[[nodiscard]] QStringList bin_labels(const QVector<double>& edges);

/// Turn lengths and look-away durations, seconds: short ones in detail,
/// long ones coarser (the distributions are skewed).
inline const QVector<double> kDurationEdgesS = {0.0, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0};

/// How a conversation's time divides: only the person on camera speaking,
/// only the other, both at once, and neither (silence), seconds, over
/// [startMs, endMs].
struct SpeakingShares {
    double subjectOnlyS = 0.0;
    double otherOnlyS   = 0.0;
    double bothS        = 0.0;
    double silenceS     = 0.0;
};
[[nodiscard]] SpeakingShares speaking_shares(const QVector<ConversationSpurt>& spurts,
                                             double startMs, double endMs);

/// Mean heart rate from the clean (NN) beat intervals falling while the person
/// on camera speaks, while the other speaks (listening), and while neither
/// does; unknown with fewer than kMinBeatsPerState intervals. A beat during
/// overlapping speech counts as both speaking and listening, as in
/// analysis/run_rppg.py's own speaking / listening rates.
struct HrByState {
    std::optional<double> speakingBpm;
    std::optional<double> listeningBpm;
    std::optional<double> silenceBpm;
    int speakingBeats  = 0;
    int listeningBeats = 0;
    int silenceBeats   = 0;
};
inline constexpr int kMinBeatsPerState = 10;
[[nodiscard]] HrByState hr_by_state(const QVector<RppgInterval>& intervals,
                                    const QVector<ConversationSpurt>& spurts);

/// Each clean interval against the next (ms): the Poincare plot of heart
/// rate variability. SD1 (short-term, beat to beat) is the spread across the
/// identity line, SD2 (longer-term) along it: SD1^2 = var(y - x) / 2 and
/// SD2^2 = var(x + y) / 2 over the plotted pairs; unknown with fewer than 3.
struct Poincare {
    QVector<QPointF> points;
    std::optional<double> sd1Ms;
    std::optional<double> sd2Ms;
};
[[nodiscard]] Poincare poincare(const QVector<RppgInterval>& intervals);

/// Blinks per minute of visible face across the video, in equal windows of
/// about @p windowMs: (window start, rate) per window, NaN for a window with
/// the face seen less than kMinFaceForRateMs (a gap, not a rate of zero).
/// Per minute of face seen, as the Face Dynamics summary counts it.
inline constexpr double kMinFaceForRateMs = 5000.0;
[[nodiscard]] QVector<QPointF> blink_rate(const QVector<FaceDynamicsEvent>& events,
                                          const QVector<FaceDynamicsFrame>& frames,
                                          double windowMs = 30000.0);

/// Words per minute of each turn with transcript words: (turn middle, rate),
/// for the person on camera and the other speaker. Turns under 1 s are left
/// out (a word or two over a fraction of a second is no rate).
struct SpeechRates {
    QVector<QPointF> subject;
    QVector<QPointF> other;
};
inline constexpr double kMinTurnForRateMs = 1000.0;
[[nodiscard]] SpeechRates speech_rates(const QVector<ConversationTurn>& turns);

/// Share of the frames with a face (the first face) per top expression, %,
/// largest first; a face without one counts as "none".
[[nodiscard]] QVector<QPair<QString, double>> expression_shares(
    const QVector<ExpressionFrame>& frames);

/// Mean intensity of each action unit (py-feat) over the frames with a face
/// (the first face); empty when the result has none.
[[nodiscard]] QVector<double> mean_action_units(const QVector<ExpressionFrame>& frames,
                                                int auCount);

} // namespace mosaic
