
.. _program_listing_file_src_video_video_encoder.hpp:

Program Listing for File video_encoder.hpp
==========================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_video_video_encoder.hpp>` (``src\video\video_encoder.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include "utils/ring_buffer.hpp"
   #include "video/frame_timestamp_writer.hpp"
   #include "video/video_frame.hpp"
   #include <QThread>
   #include <memory>
   
   namespace mosaic {
   
   // Consumes VideoFrames from a ring buffer and encodes them to an MP4 file.
   //
   // Codec selection at compile time:
   //   MOSAIC_HAVE_NVENC        → h264_nvenc  (NVIDIA GPU, Windows/Linux)
   //   MOSAIC_HAVE_VIDEOTOOLBOX → h264_videotoolbox (Apple, zero-copy)
   //   neither                  → libx264 software fallback
   //
   // When FFmpeg is not present at all (no MOSAIC_HAVE_NVENC and not Apple),
   // the encoder writes a minimal raw-stream marker file and logs a warning.
   //
   // FrameTimestampWriter is owned by the encoder: it is started before the
   // encode loop and stopped when encoding finishes.
   
   class VideoEncoder : public QThread {
       Q_OBJECT
   public:
       struct Config {
           int     cameraIndex  = 0;
           QString outputPath;          // .mp4 file
           QString timestampPath;       // timestamps_camN.csv
           QString codec        = "h264_nvenc";
           QString preset       = "p4";
           int     bitrate      = 5000; // kbit/s  (GPU encoder)
           int     crf          = 23;   // quality  (CPU encoder, 0=lossless, 51=worst)
           int     width        = 1920;
           int     height       = 1080;
           double  fps          = 30.0;
       };
   
       explicit VideoEncoder(const Config&                           cfg,
                             RingBuffer<std::shared_ptr<VideoFrame>>& frameBuffer,
                             QObject*                                parent = nullptr);
       ~VideoEncoder() override;
   
       void start_encoding();
       void stop_encoding();   // sets stop flag; call wait() to join
   
       [[nodiscard]] int64_t frames_encoded() const;
       [[nodiscard]] int64_t frames_dropped() const;
   
   signals:
       void encoding_started(int cameraIndex);
       void encoding_stopped(int cameraIndex, int64_t totalFrames);
       void encoding_error(int cameraIndex, QString message);
   
   protected:
       void run() override;
   
   private:
       void run_ffmpeg_loop();
       void run_stub_loop();
   
       struct Impl;
       std::unique_ptr<Impl> d;
   };
   
   } // namespace mosaic
