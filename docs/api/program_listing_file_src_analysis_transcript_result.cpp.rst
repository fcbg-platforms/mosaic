
.. _program_listing_file_src_analysis_transcript_result.cpp:

Program Listing for File transcript_result.cpp
==============================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_analysis_transcript_result.cpp>` (``src\analysis\transcript_result.cpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #include "analysis/transcript_result.hpp"
   #include <QFile>
   #include <QJsonArray>
   #include <QJsonDocument>
   #include <QJsonObject>
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
   
       for (const auto& segVal : root["segments"].toArray()) {
           const QJsonObject segObj = segVal.toObject();
   
           TranscriptSegment seg;
           seg.startMs = static_cast<int64_t>(segObj["start_ms"].toDouble());
           seg.endMs   = static_cast<int64_t>(segObj["end_ms"].toDouble());
           seg.speaker = segObj["speaker"].toString();   // JSON null -> empty QString
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
           return nullptr;   // ms is before the first segment even starts
       }
       const auto prevIt = std::prev(it);
       if (ms >= prevIt->startMs && ms < prevIt->endMs) {
           return &(*prevIt);
       }
       return nullptr;   // ms falls in a gap between segments
   }
   
   } // namespace mosaic
