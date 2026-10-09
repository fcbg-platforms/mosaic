#pragma once
#include <QString>
#include <QVector>
#include <cstdint>
#include <optional>

namespace mosaic {

/// One turn: consecutive speech of one speaker until the other takes the
/// floor. speaker is "subject" (the face on camera) or "other".
struct ConversationTurn {
    QString speaker;
    int64_t startMs = 0;
    int64_t endMs   = 0;
    int pauses      = 0;
    QString text; ///< Transcript words, empty without Speaker Diarization.
};

/// A change of turn. ftoMs (floor transfer offset) is the new turn's start
/// minus the end of the previous speaker's speech: positive a gap, negative
/// an overlap.
struct ConversationTransition {
    QString from;
    QString to;
    int64_t prevEndMs   = 0;
    int64_t nextStartMs = 0;
    int ftoMs           = 0;
    bool interruption   = false;
};

/// A stretch of one speaker's speech. kind is "turn", "backchannel" or
/// "overlap" (spoken inside the other's speech without taking the floor).
struct ConversationSpurt {
    QString speaker;
    int64_t startMs = 0;
    int64_t endMs   = 0;
    QString kind;
};

/// Per-speaker numbers of the "summary.speakers" object.
struct ConversationSpeakerStats {
    double speechS = 0.0;
    std::optional<double> speechPct;
    int turns = 0;
    std::optional<double> medianTurnS;
    int pauses = 0;
    std::optional<double> pausesPerMin;
    int backchannels = 0;
    std::optional<double> medianResponseS; ///< Median FTO of transitions to this speaker.
    int interruptions = 0;                 ///< Transitions to this speaker that interrupted.
    std::optional<double> wordsPerMin;
};

/// Parses a "<video_stem>.conversation.json" written by
/// analysis/run_conversation.py (schema mosaic-conversation-v1), for the
/// Analysis tab's Conversation Timing plugin. Times are elapsed_ms, the
/// clock of the video's timestamps; videoStartMs is the video's first frame.
class ConversationResult {
   public:
    ConversationResult() = default;

    /// Invalid when the file is missing, malformed or of another schema.
    static ConversationResult load(const QString& jsonPath);

    [[nodiscard]] bool is_valid() const { return valid_; }
    [[nodiscard]] const QString& source_video() const { return sourceVideo_; }
    [[nodiscard]] const QString& source_audio() const { return sourceAudio_; }
    [[nodiscard]] int camera_index() const { return cameraIndex_; }
    [[nodiscard]] double video_start_ms() const { return videoStartMs_; }
    [[nodiscard]] const QString& annotated_video() const { return annotatedVideo_; }
    /// "timing_file", "session_start" or "session_start+av_lag".
    [[nodiscard]] const QString& timing_method() const { return timingMethod_; }
    /// "diarization" or "mouth".
    [[nodiscard]] const QString& attribution_method() const { return attributionMethod_; }

    [[nodiscard]] const QVector<ConversationTurn>& turns() const { return turns_; }
    [[nodiscard]] const QVector<ConversationTransition>& transitions() const {
        return transitions_;
    }
    [[nodiscard]] const QVector<ConversationSpurt>& spurts() const { return spurts_; }

    [[nodiscard]] const ConversationSpeakerStats& subject() const { return subject_; }
    [[nodiscard]] const ConversationSpeakerStats& other() const { return other_; }
    [[nodiscard]] int transition_count() const { return transitionCount_; }
    [[nodiscard]] std::optional<double> median_fto_s() const { return medianFtoS_; }
    [[nodiscard]] int gaps() const { return gaps_; }
    [[nodiscard]] int overlapping_transitions() const { return overlapping_; }
    [[nodiscard]] double overlap_total_s() const { return overlapTotalS_; }
    [[nodiscard]] std::optional<double> silence_pct() const { return silencePct_; }
    [[nodiscard]] double face_seen_pct() const { return faceSeenPct_; }

    /// Chart series sampled every signal_step_ms() from signal_start_ms();
    /// NaN where the file has null (no face).
    [[nodiscard]] double signal_start_ms() const { return signalStartMs_; }
    [[nodiscard]] double signal_step_ms() const { return signalStepMs_; }
    [[nodiscard]] const QVector<double>& audio_db() const { return audioDb_; }
    [[nodiscard]] const QVector<double>& mouth_activity() const { return mouthActivity_; }

   private:
    bool valid_ = false;
    QString sourceVideo_;
    QString sourceAudio_;
    int cameraIndex_     = 0;
    double videoStartMs_ = 0.0;
    QString annotatedVideo_;
    QString timingMethod_;
    QString attributionMethod_;
    QVector<ConversationTurn> turns_;
    QVector<ConversationTransition> transitions_;
    QVector<ConversationSpurt> spurts_;
    ConversationSpeakerStats subject_;
    ConversationSpeakerStats other_;
    int transitionCount_ = 0;
    std::optional<double> medianFtoS_;
    int gaps_             = 0;
    int overlapping_      = 0;
    double overlapTotalS_ = 0.0;
    std::optional<double> silencePct_;
    double faceSeenPct_   = 0.0;
    double signalStartMs_ = 0.0;
    double signalStepMs_  = 0.0;
    QVector<double> audioDb_;
    QVector<double> mouthActivity_;
};

} // namespace mosaic
