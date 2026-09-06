#pragma once
#include <QString>
#include <QVector>
#include <cstdint>

namespace mosaic {

/// One transcribed segment. Mirrors run_diarize.py's assign_speakers() output
/// schema exactly (see analysis/diarize/pipeline.py).
struct TranscriptSegment {
    int64_t startMs = 0;
    int64_t endMs   = 0;
    QString speaker; ///< Empty = no diarization turn overlapped, or diarization wasn't run.
    QString text;
};

/// Why a transcript did or didn't get speaker labels.
///
/// Mirrors DIARIZATION_STATUSES in analysis/diarize/pipeline.py; the two must
/// be kept in step. Exists because "diarization: false" alone is unactionable:
/// no Hugging Face token, a gated model refusing to load, and a model that ran
/// and heard nobody all produce a transcript with every speaker empty, which
/// renders identically — a blank Speaker column and an unshaded waveform. The
/// reason used to live only in the run's stdout, which has scrolled away by
/// the time anyone wonders.
enum class DiarizationStatus {
    /// No status key in the file: written before this field existed. Falls
    /// back to has_diarization() for wording rather than guessing a reason.
    Unknown,
    Ok,
    SkippedByUser, ///< "Transcript only" was ticked.
    NoToken,       ///< No Hugging Face token was supplied.
    LoadFailed,    ///< The pipeline would not load — usually gated-model terms.
    RunFailed,     ///< It loaded but threw while diarizing this file.
    NoTurns,       ///< It ran and heard nobody.
    /// It heard people, but none of their turns overlapped a transcribed
    /// segment. Distinct from NoTurns because the cause and the advice
    /// differ: the microphone did capture speech.
    NoOverlap,
};

/// One-line statement of what happened, for a UI banner. Empty for Ok.
/// @param hasDiarization  The file's legacy boolean, used only when the status
///                        is Unknown.
[[nodiscard]] QString diarization_status_headline(DiarizationStatus status, bool hasDiarization);

/// What the operator should actually do about it — the half a bare error
/// message always leaves out. Empty when there is nothing useful to say.
[[nodiscard]] QString diarization_status_remedy(DiarizationStatus status);

/// Parses a "<name>.transcript.json" file written by analysis/run_diarize.py
/// into a queryable in-memory structure, for the Analysis tab's Speaker
/// Diarization plugin (transcript table + playback-synced highlighting).
///
/// Usage:
/// @code
///   auto result = TranscriptResult::load(jsonPath);
///   if (result.is_valid()) { ... }
/// @endcode
class TranscriptResult {
   public:
    TranscriptResult() = default;

    /// Parses jsonPath. Returns a default-constructed (is_valid() == false)
    /// result if the file is missing or malformed.
    static TranscriptResult load(const QString& jsonPath);

    [[nodiscard]] bool is_valid() const { return valid_; }
    [[nodiscard]] bool has_diarization() const { return hasDiarization_; }

    /// Why speaker labels are or aren't present. Unknown for files written
    /// before this field existed.
    [[nodiscard]] DiarizationStatus diarization_status() const { return diarizationStatus_; }

    /// The Python side's own sentence about it — for LoadFailed this is the
    /// verbatim exception text, which is the only thing that distinguishes a
    /// bad token from an unaccepted licence. May be empty.
    [[nodiscard]] const QString& diarization_detail() const { return diarizationDetail_; }
    [[nodiscard]] const QString& source_audio() const { return sourceAudio_; }
    [[nodiscard]] const QString& language() const { return language_; }
    [[nodiscard]] const QVector<TranscriptSegment>& segments() const { return segments_; }

    /// Binary search by start time (segments() is chronological, matching
    /// run_diarize.py's write order). Returns nullptr if ms falls in a gap
    /// between segments, before the first, or after the last one ends.
    [[nodiscard]] const TranscriptSegment* segment_at(int64_t ms) const;

   private:
    bool valid_                          = false;
    bool hasDiarization_                 = false;
    DiarizationStatus diarizationStatus_ = DiarizationStatus::Unknown;
    QString diarizationDetail_;
    QString sourceAudio_;
    QString language_;
    QVector<TranscriptSegment> segments_;
};

} // namespace mosaic
