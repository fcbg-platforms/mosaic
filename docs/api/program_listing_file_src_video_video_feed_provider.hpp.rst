
.. _program_listing_file_src_video_video_feed_provider.hpp:

Program Listing for File video_feed_provider.hpp
================================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_video_video_feed_provider.hpp>` (``src\video\video_feed_provider.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include <QImage>
   #include <QReadWriteLock>
   #include <QQuickImageProvider>
   #include <vector>
   
   namespace mosaic {
   
   // Provides per-camera BGR preview frames to QML via the "videofeed" image provider.
   // Thread-safe: update_frame() is called from the main thread after a queued
   // signal hop from the grabber thread.
   //
   // QML usage:
   //   Image { cache: false; source: "image://videofeed/" + cameraIndex + "?v=" + backend.frameGen }
   //   Changing the ?v= query string forces a re-request even for the same index.
   class VideoFeedProvider : public QQuickImageProvider {
   public:
       VideoFeedProvider();
   
       // Called by the image loader thread — must be thread-safe.
       QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
   
       // Called from the main thread when a new preview frame arrives.
       void update_frame(int cameraIndex, const QImage& img);
   
       // Pre-allocate slots so requestImage never returns an index-out-of-range image.
       void set_camera_count(int count);
   
   private:
       mutable QReadWriteLock m_lock;
       std::vector<QImage>    m_frames;
   };
   
   } // namespace mosaic
