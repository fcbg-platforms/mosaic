
.. _program_listing_file_src_audio_audio_envelope.hpp:

Program Listing for File audio_envelope.hpp
===========================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_audio_audio_envelope.hpp>` (``src\audio\audio_envelope.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include <QByteArray>
   #include <QtTypes>
   
   namespace mosaic {
   
   // Pure signal-analysis helpers over raw signed 16-bit PCM — no QtMultimedia
   // dependency, so these are directly unit-testable (mirrors src/calibration/
   // rms_quality.hpp's "pure function, no hardware dependency" pattern).
   // Both treat `data` as flat interleaved int16_t regardless of channel count
   // (same simplification the original AudioRecorder::compute_rms() made).
   
   // Normalized [-1, 0] / [0, 1] min/max envelope of one PCM buffer — the
   // standard "zoomed-out audio-editor waveform" technique: the buffer's most
   // negative and most positive sample, each divided by 32768. Both fields are
   // 0 for an empty/null buffer. minSample <= maxSample always holds since both
   // are seeded from the buffer's own first sample, never from a fixed 0.
   struct AudioEnvelope {
       float minSample = 0.0f;
       float maxSample = 0.0f;
   };
   
   [[nodiscard]] AudioEnvelope compute_envelope(const char* data, qint64 bytes);
   
   // Root-mean-square level in [0, 1]. 0 for an empty/null buffer.
   [[nodiscard]] float compute_rms(const char* data, qint64 bytes);
   
   // Format-conversion helpers, needed because not every audio interface
   // supports 16-bit PCM capture at all — many professional/USB Audio Class 2.0
   // devices only offer 32-bit float or 32-bit int. AudioRecorder captures in
   // whichever format the device actually supports and converts every buffer to
   // 16-bit PCM via one of these before it ever reaches WavWriter/compute_rms/
   // compute_envelope, so nothing downstream needs to know or care what format
   // the hardware captured in. Kept here (not in AudioRecorder) specifically so
   // they stay pure and unit-testable without a live QAudioSource.
   
   // Converts 32-bit float PCM (each sample nominally in [-1, 1], native byte
   // order) to 16-bit signed PCM, clamping out-of-range values before scaling.
   [[nodiscard]] QByteArray convert_float32_to_int16(const char* data, qint64 bytes);
   
   // Converts 32-bit signed int PCM (full int32 range) to 16-bit signed PCM via
   // a truncating right-shift (keeps the most-significant 16 bits, matching how
   // audio interfaces typically left-justify lower-resolution samples e.g.
   // 24-bit-in-32-bit into the top of the word).
   [[nodiscard]] QByteArray convert_int32_to_int16(const char* data, qint64 bytes);
   
   } // namespace mosaic
