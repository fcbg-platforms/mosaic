
.. _program_listing_file_src_trigger_trigger_recorder.hpp:

Program Listing for File trigger_recorder.hpp
=============================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_trigger_trigger_recorder.hpp>` (``src/trigger/trigger_recorder.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include "trigger/trigger_types.hpp"
   #include <QString>
   #include <memory>
   
   namespace mosaic {
   
   // Writes TriggerEvents to a CSV file during a recording session.
   // Thread-safe: record_event() can be called from any thread.
   
   class TriggerRecorder {
   public:
       TriggerRecorder();
       ~TriggerRecorder();
   
       // Opens the CSV file. Returns false on I/O error.
       [[nodiscard]] bool start(const QString& path);
   
       // Writes one event row. Safe to call from any thread.
       void record_event(const TriggerEvent& event);
   
       // Flushes and closes the file.
       void stop();
   
       [[nodiscard]] bool is_recording() const;
   
   private:
       struct Impl;
       std::unique_ptr<Impl> d;
   };
   
   } // namespace mosaic
