
.. _program_listing_file_src_video_video_frame.hpp:

Program Listing for File video_frame.hpp
========================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_video_video_frame.hpp>` (``src\video\video_frame.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include <cstdint>
   #include <vector>
   
   namespace mosaic {
   
   /// @brief An immutable snapshot of one camera grab.
   ///
   /// Use @c std::shared_ptr<VideoFrame> (or @c std::shared_ptr<const VideoFrame>)
   /// to move frames across the grabber → ring-buffer → encoder pipeline without
   /// copying the pixel data.
   ///
   /// Pixel format: **BGR8** — 3 bytes per pixel in Blue, Green, Red order.
   /// This is the native Pylon output format and is also what OpenCV and most
   /// FFmpeg encoders expect after a @c sws_scale() call.
   ///
   /// @par Timestamp fields
   /// Three timestamps are captured at (or derived from) the moment of the
   /// camera grab (inside VideoGrabber, before the ring-buffer push):
   ///
   /// | Field          | Clock                  | Use case                                   |
   /// |----------------|------------------------|---------------------------------------------|
   /// | @c elapsedNs   | steady_clock (host)    | Cross-camera alignment (SyncManifest)      |
   /// | @c wallClockNs | system_clock (host)    | Absolute calendar alignment; file naming   |
   /// | @c hwTimestampNs | camera device clock  | Diagnostic ground-truth; per-camera only   |
   ///
   /// @c hwTimestampNs comes from the Basler camera's own GevTimestamp chunk
   /// (converted from ticks to nanoseconds using GevTimestampTickFrequency) —
   /// it is captured by the camera at exposure time rather than stamped in
   /// software after RetrieveResult() returns, so it is not subject to host
   /// scheduling/network jitter. However each camera's device clock is free-
   /// running relative to the others unless hardware triggering is active, so
   /// it is **not** directly comparable across cameras and must not be used
   /// for cross-camera alignment on its own — use @c elapsedNs for that.
   /// It is 0 if chunk timestamps could not be read from this frame.
   ///
   /// All three are written to @c timestamps_camN.csv by FrameTimestampWriter.
   struct VideoFrame {
       int     cameraIndex   = 0;    ///< Zero-based camera index (matches timestamps_camN.csv).
       int64_t frameId       = 0;    ///< Monotonic per-camera counter starting at 1 per session.
       int64_t elapsedNs     = 0;    ///< elapsed_ns() at grab time — steady_clock.
       int64_t wallClockNs   = 0;    ///< wall_clock_ns() at grab time — system_clock.
       int64_t hwTimestampNs = 0;    ///< Camera-hardware chunk timestamp, ns. 0 if unavailable.
   
       int     width        = 0;    ///< Frame width in pixels.
       int     height       = 0;    ///< Frame height in pixels.
       int     stride       = 0;    ///< Bytes per row (width × 3 for BGR8, may include padding).
   
       /// Raw pixel bytes in BGR8 order (3 bytes per pixel, row-major).
       std::vector<uint8_t> data;
   
       /// @returns @c true if the frame has non-zero dimensions and non-empty data.
       [[nodiscard]] bool   is_valid()   const noexcept { return width > 0 && height > 0 && !data.empty(); }
   
       /// @returns Total number of bytes in the pixel data (stride × height).
       [[nodiscard]] size_t byte_count() const noexcept { return static_cast<size_t>(stride * height); }
   };
   
   } // namespace mosaic
