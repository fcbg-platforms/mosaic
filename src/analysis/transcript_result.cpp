#include "analysis/transcript_result.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <algorithm>

namespace mosaic {

TranscriptResult TranscriptResult::load(const QString& jsonPath) {
    TranscriptResult result;

    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return result;
    }

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.isEmpty()) {
        return result;
    }

    result.sourceAudio_    = root["source_audio"].toString();
    result.language_       = root["language"].toString();
    result.hasDiarization_ = root["diarization"].toBool();

    // Absent in files written before this field existed; those keep
    // DiarizationStatus::Unknown so the UI words itself from the boolean
    // instead of asserting a reason nobody recorded. An unrecognised value
    // maps to Unknown too — a future Python status this build doesn't know
    // must not be silently read as one it does.
    static const QMap<QString, DiarizationStatus> kStatuses{
        {"ok", DiarizationStatus::Ok},
        {"skipped_by_user", DiarizationStatus::SkippedByUser},
        {"no_token", DiarizationStatus::NoToken},
        {"load_failed", DiarizationStatus::LoadFailed},
        {"run_failed", DiarizationStatus::RunFailed},
        {"no_turns", DiarizationStatus::NoTurns},
        {"no_overlap", DiarizationStatus::NoOverlap},
    };
    result.diarizationStatus_ =
        kStatuses.value(root["diarization_status"].toString(), DiarizationStatus::Unknown);
    result.diarizationDetail_ = root["diarization_detail"].toString();

    for (const auto& segVal : root["segments"].toArray()) {
        const QJsonObject segObj = segVal.toObject();

        TranscriptSegment seg;
        seg.startMs = static_cast<int64_t>(segObj["start_ms"].toDouble());
        seg.endMs   = static_cast<int64_t>(segObj["end_ms"].toDouble());
        seg.speaker = segObj["speaker"].toString(); // JSON null -> empty QString
        seg.text    = segObj["text"].toString();

        result.segments_ << seg;
    }

    // segment_at() binary-searches assuming ascending startMs. Segments are
    // normally written in chronological order by run_diarize.py, but
    // faster-whisper can occasionally emit non-monotonic timestamps near
    // VAD/hallucination boundaries — sort defensively rather than trust an
    // external library's ordering.
    std::sort(result.segments_.begin(), result.segments_.end(),
              [](const TranscriptSegment& a, const TranscriptSegment& b) {
                  return a.startMs < b.startMs;
              });

    result.valid_ = true;
    return result;
}

const TranscriptSegment* TranscriptResult::segment_at(int64_t ms) const {
    if (segments_.isEmpty()) {
        return nullptr;
    }

    // Last segment whose startMs <= ms.
    const auto it = std::upper_bound(
        segments_.begin(), segments_.end(), ms,
        [](int64_t value, const TranscriptSegment& s) { return value < s.startMs; });

    if (it == segments_.begin()) {
        return nullptr; // ms is before the first segment even starts
    }
    const auto prevIt = std::prev(it);
    if (ms >= prevIt->startMs && ms < prevIt->endMs) {
        return &(*prevIt);
    }
    return nullptr; // ms falls in a gap between segments
}

QString diarization_status_headline(DiarizationStatus status, bool hasDiarization) {
    switch (status) {
        case DiarizationStatus::Ok:
            return {};
        case DiarizationStatus::SkippedByUser:
            return QStringLiteral(
                "Transcript only — speaker labelling was turned off for this run.");
        case DiarizationStatus::NoToken:
            return QStringLiteral("No speaker labels: diarization needs a Hugging Face token.");
        case DiarizationStatus::LoadFailed:
            return QStringLiteral("No speaker labels: the diarization model could not be loaded.");
        case DiarizationStatus::RunFailed:
            return QStringLiteral("No speaker labels: diarization failed while running.");
        case DiarizationStatus::NoTurns:
            return QStringLiteral(
                "No speaker labels: the model found no speaker turns in this audio.");
        case DiarizationStatus::NoOverlap:
            return QStringLiteral(
                "No speaker labels: the speaker turns found didn't line up with the transcript.");
        case DiarizationStatus::Unknown:
            break;
    }
    // Pre-status file. If it says diarization ran, take it at its word and say
    // nothing; otherwise all that can honestly be reported is that labels are
    // missing. Inventing a cause here would be worse than admitting none was
    // recorded — this file predates the field that would have held it.
    if (hasDiarization) {
        return {};
    }
    return QStringLiteral(
        "No speaker labels in this transcript (it was produced before "
        "MOSAIC recorded why).");
}

QString diarization_status_remedy(DiarizationStatus status) {
    switch (status) {
        case DiarizationStatus::SkippedByUser:
            return QStringLiteral("Untick \"Transcript only\" and run again.");
        case DiarizationStatus::NoToken:
        case DiarizationStatus::LoadFailed:
            // Both steps, deliberately. A token on its own is not enough: the
            // model is gated, and a token whose owner never accepted its terms
            // fails at load with a 401 that looks like a bad token.
            return QStringLiteral(
                "Paste a Hugging Face token above, and accept the terms for "
                "pyannote/speaker-diarization-community-1 at huggingface.co, then run again.");
        case DiarizationStatus::RunFailed:
            return QStringLiteral(
                "See the log for the failure; re-running often clears a "
                "transient download or memory error.");
        case DiarizationStatus::NoTurns:
            return QStringLiteral(
                "Check the microphone actually captured speech — the waveform "
                "above shows what was recorded.");
        case DiarizationStatus::NoOverlap:
            // Deliberately not the NoTurns advice: speech *was* captured, so
            // telling the operator to check the microphone would send them
            // after the wrong thing.
            return QStringLiteral(
                "Speech was detected but couldn't be matched to the transcript — "
                "re-running, or a larger Whisper model, usually resolves it.");
        case DiarizationStatus::Ok:
        case DiarizationStatus::Unknown:
            break;
    }
    return {};
}

} // namespace mosaic
