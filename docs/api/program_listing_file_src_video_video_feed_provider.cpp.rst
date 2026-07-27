
.. _program_listing_file_src_video_video_feed_provider.cpp:

Program Listing for File video_feed_provider.cpp
================================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_video_video_feed_provider.cpp>` (``src\video\video_feed_provider.cpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #include "video/video_feed_provider.hpp"
   
   namespace mosaic {
   
   VideoFeedProvider::VideoFeedProvider()
       : QQuickImageProvider(QQuickImageProvider::Image) {}
   
   QImage VideoFeedProvider::requestImage(const QString& id,
                                           QSize* size,
                                           const QSize& /*requestedSize*/) {
       // id is "<index>" or "<index>?v=<gen>"; strip the query string.
       const int idx = id.split('?').first().toInt();
   
       QReadLocker lock(&m_lock);
       if (idx >= 0 && idx < static_cast<int>(m_frames.size()) && !m_frames[idx].isNull()) {
           const QImage& img = m_frames[idx];
           if (size) *size = img.size();
           return img;
       }
   
       // Return a solid dark placeholder so QML shows something immediately.
       // QImage's (width, height, format) constructor allocates an
       // *uninitialized* buffer — without fill(), this "placeholder" was
       // whatever memory happened to be there, which reads as random-looking
       // colour noise once scaled up to fill a tile (indistinguishable from a
       // real but corrupted frame).
       static const QImage placeholder = [] {
           QImage img(4, 4, QImage::Format_BGR888);
           img.fill(Qt::black);
           return img;
       }();
       if (size) *size = placeholder.size();
       return placeholder;
   }
   
   void VideoFeedProvider::update_frame(int idx, const QImage& img) {
       QWriteLocker lock(&m_lock);
       if (idx >= static_cast<int>(m_frames.size()))
           m_frames.resize(static_cast<size_t>(idx) + 1);
       m_frames[static_cast<size_t>(idx)] = img;
   }
   
   void VideoFeedProvider::set_camera_count(int count) {
       QWriteLocker lock(&m_lock);
       m_frames.resize(static_cast<size_t>(count));
   }
   
   } // namespace mosaic
