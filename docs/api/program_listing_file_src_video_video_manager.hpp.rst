
.. _program_listing_file_src_video_video_manager.hpp:

Program Listing for File video_manager.hpp
==========================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_video_video_manager.hpp>` (``src/video/video_manager.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include "core/settings.hpp"
   #include "video/video_frame.hpp"
   #include <QObject>
   #include <memory>
   
   namespace mosaic {
   
   /// @brief Orchestrates one VideoGrabber + VideoEncoder pair per configured camera.
   ///
   /// VideoManager owns the complete video pipeline for a session.  It matches
   /// the lifecycle of the cameras: open them once at startup, then start/stop
   /// recording repeatedly without reopening.
   ///
   /// @par Thread model
   /// Each camera runs two background threads:
   /// - **VideoGrabber** — grabs frames from Pylon (or generates test patterns).
   /// - **VideoEncoder** — encodes frames via FFmpeg and writes the timestamp CSV.
   ///
   /// Frames are passed between threads through a lock-free SPSC RingBuffer.
   /// All *public* methods of VideoManager itself must be called from the main
   /// thread.
   ///
   /// @par Lifecycle
   /// @code{.cpp}
   /// VideoManager vm;
   /// int opened = vm.open(settings.video);   // opens cameras once
   ///
   /// // Start/stop can repeat throughout the session
   /// vm.start("recordings/2026-06-04_14-32-05", "video", settings.video);
   /// // ... recording in progress ...
   /// vm.stop();   // blocks until all encoders have flushed and closed their files
   ///
   /// vm.close();  // release camera handles (called by destructor)
   /// @endcode
   ///
   /// @see VideoGrabber, VideoEncoder, FrameTimestampWriter, RecordManager
   class VideoManager : public QObject {
       Q_OBJECT
   public:
       explicit VideoManager(QObject* parent = nullptr);
       ~VideoManager() override;
   
       /// @brief Opens camera devices (or stub generators) for all configured cameras.
       ///
       /// @param settings  Video settings from AppSettings.  The @c cameras vector
       ///                  determines how many devices are opened.
       /// @returns         The number of cameras successfully opened.  May be less
       ///                  than @c settings.cameras.size() if some devices fail.
       int  open(const VideoSettings& settings);
   
       /// @brief Closes all open camera handles.
       ///
       /// Calls stop() first if a recording is active.
       void close();
   
       /// @brief Starts grabbing and encoding for all open cameras.
       ///
       /// Output files:
       /// - @c \<sessionDir\>/\<videoBasename\>_N.mp4
       /// - @c \<sessionDir\>/timestamps_camN.csv
       ///
       /// @param sessionDir    Absolute path to the session folder (must exist).
       /// @param videoBasename Basename for video files (e.g. @c "video").
       /// @param settings      Video settings (codec, preset, per-camera FPS/resolution).
       void start(const QString&       sessionDir,
                  const QString&       videoBasename,
                  const VideoSettings& settings);
   
       /// @brief Stops all grabbers and encoders, flushing and closing every file.
       ///
       /// Blocks until all encoder threads have exited (up to 10 s per camera).
       void stop();
   
       /// @returns @c true while a recording session is active.
       [[nodiscard]] bool    is_recording()          const;
   
       /// @returns The number of cameras that were successfully opened.
       [[nodiscard]] int     camera_count()           const;
   
       /// @returns Total frames encoded across all cameras since the last start().
       [[nodiscard]] int64_t total_frames_encoded()   const;
   
       /// @returns Total frames dropped (ring buffer overflow) since last start().
       [[nodiscard]] int64_t total_frames_dropped()   const;
   
   signals:
       /// Emitted on the main thread when a camera device is successfully opened.
       void camera_opened(int cameraIndex, int width, int height, double fps);
   
       /// Emitted when a camera device is closed.
       void camera_closed(int cameraIndex);
   
       /// Emitted each time a frame is dropped because the ring buffer was full.
       /// @param cameraIndex  Which camera dropped the frame.
       /// @param frameId      The frame counter value of the dropped frame.
       void frame_dropped(int cameraIndex, int64_t frameId);
   
       /// Emitted when a camera grab or encode error occurs.
       void camera_error(int cameraIndex, QString message);
   
       /// Emitted when all encoders have finished (files closed and flushed).
       void recording_stopped();
   
   private slots:
       void on_encoder_stopped(int cameraIndex, int64_t frames);
   
   private:
       struct Impl;
       std::unique_ptr<Impl> d;
   };
   
   } // namespace mosaic
