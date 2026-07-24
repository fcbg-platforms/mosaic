
.. _program_listing_file_src_analysis_transcript_result.hpp:

Program Listing for File transcript_result.hpp
==============================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_analysis_transcript_result.hpp>` (``src\analysis\transcript_result.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include <QString>
   #include <QVector>
   #include <cstdint>
   
   namespace mosaic {
   
   // One transcribed segment. Mirrors run_diarize.py's assign_speakers() output
   // schema exactly (see analysis/diarize/pipeline.py).
   struct TranscriptSegment {
       int64_t startMs = 0;
       int64_t endMs   = 0;
       QString speaker;   // empty = no diarization turn overlapped, or diarization wasn't run
       QString text;
   };
   
   // Parses a "<name>.transcript.json" file written by analysis/run_diarize.py
   // into a queryable in-memory structure, for the Analysis tab's Speaker
   // Diarization plugin (transcript table + playback-synced highlighting).
   //
   // Usage:
   //   auto result = TranscriptResult::load(jsonPath);
   //   if (result.is_valid()) { ... }
   class TranscriptResult {
   public:
       TranscriptResult() = default;
   
       // Parses jsonPath. Returns a default-constructed (is_valid() == false)
       // result if the file is missing or malformed.
       static TranscriptResult load(const QString& jsonPath);
   
       [[nodiscard]] bool is_valid() const { return valid_; }
       [[nodiscard]] bool has_diarization() const { return hasDiarization_; }
       [[nodiscard]] const QString& source_audio() const { return sourceAudio_; }
       [[nodiscard]] const QString& language() const { return language_; }
       [[nodiscard]] const QVector<TranscriptSegment>& segments() const { return segments_; }
   
       // Binary search by start time (segments() is chronological, matching
       // run_diarize.py's write order). Returns nullptr if ms falls in a gap
       // between segments, before the first, or after the last one ends.
       [[nodiscard]] const TranscriptSegment* segment_at(int64_t ms) const;
   
   private:
       bool                        valid_          = false;
       bool                        hasDiarization_ = false;
       QString                     sourceAudio_;
       QString                     language_;
       QVector<TranscriptSegment>  segments_;
   };
   
   } // namespace mosaic
