
.. _program_listing_file_src_video_frame_timestamp_writer.hpp:

Program Listing for File frame_timestamp_writer.hpp
===================================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_video_frame_timestamp_writer.hpp>` (``src/video/frame_timestamp_writer.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include <QString>
   #include <cstdint>
   #include <memory>
   
   namespace mosaic {
   
   // Writes one CSV row per grabbed frame:
   //   frame_id, elapsed_ns, wall_ns
   //
   // Thread-safe: write() is called from the grabber thread while the main
   // thread may call is_open() / frames_written() concurrently.
   // The file is flushed and closed in stop() from whatever thread calls it.
   
   class FrameTimestampWriter {
   public:
       FrameTimestampWriter();
       ~FrameTimestampWriter();
   
       // Opens the CSV and writes the header row.
       [[nodiscard]] bool start(const QString& path);
   
       // Appends one row. Thread-safe.
       void write(int64_t frameId, int64_t elapsedNs, int64_t wallNs);
   
       // Flushes and closes the file.
       void stop();
   
       [[nodiscard]] bool    is_open()        const;
       [[nodiscard]] int64_t frames_written() const;
   
   private:
       struct Impl;
       std::unique_ptr<Impl> d;
   };
   
   } // namespace mosaic
