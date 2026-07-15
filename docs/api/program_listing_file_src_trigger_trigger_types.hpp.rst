
.. _program_listing_file_src_trigger_trigger_types.hpp:

Program Listing for File trigger_types.hpp
==========================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_trigger_trigger_types.hpp>` (``src/trigger/trigger_types.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include <QString>
   #include <cstdint>
   
   namespace mosaic {
   
   /// @brief A single timestamped event from any trigger source.
   ///
   /// TriggerEvent is the common currency passed between trigger sources
   /// (KeyboardTrigger, LslInlet, ParallelPortTrigger), TriggerManager,
   /// and TriggerRecorder.  It is also emitted by TriggerManager::event_received()
   /// so the UI or any downstream consumer can react in real time.
   ///
   /// @par CSV columns
   /// TriggerRecorder writes one row per event:
   /// @code
   /// elapsed_ms,wall_clock,source,label,value
   /// 1523,14:32:06.645,keyboard,Event A,0
   /// 2100,14:32:07.222,lsl,Stimulus/S1,1
   /// 4910,14:32:09.032,parallel_port,D3_RISE,1
   /// @endcode
   struct TriggerEvent {
       /// Monotonic nanosecond timestamp from elapsed_ns() at the moment the
       /// event was detected.  Use this to align to @c timestamps_camN.csv.
       int64_t timestampNs = 0;
   
       /// Where the event originated.  One of:
       /// - @c "keyboard"       — a KeyboardTrigger event filter.
       /// - @c "lsl"            — a received LSL marker.
       /// - @c "parallel_port"  — a bit-edge on the LPT data register.
       QString source;
   
       /// Human-readable event name.
       /// - Keyboard: the user-configured binding name, e.g. @c "Event A".
       /// - LSL:      the received string sample, e.g. @c "Stimulus/S1".
       /// - Parallel: bit and edge, e.g. @c "D3_RISE" or @c "D3_FALL".
       QString label;
   
       /// Optional numeric payload.
       /// - Keyboard / parallel rising edge: @c 1.0.
       /// - Parallel falling edge:           @c 0.0.
       /// - LSL:                             @c 0.0 (string-only inlet).
       double  value = 0.0;
   };
   
   } // namespace mosaic
