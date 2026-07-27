
.. _program_listing_file_src_trigger_lsl_outlet.cpp:

Program Listing for File lsl_outlet.cpp
=======================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_trigger_lsl_outlet.cpp>` (``src\trigger\lsl_outlet.cpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #include "trigger/lsl_outlet.hpp"
   #include "utils/logger.hpp"
   
   #if defined(MOSAIC_HAVE_LSL)
   #  include <lsl_cpp.h>
   #endif
   
   namespace mosaic {
   
   struct LslOutlet::Impl {
       bool open{false};
   
   #if defined(MOSAIC_HAVE_LSL)
       std::unique_ptr<lsl::stream_outlet> videoOutlet;
       std::unique_ptr<lsl::stream_outlet> eventOutlet;
   #endif
   };
   
   LslOutlet::LslOutlet()  : d(std::make_unique<Impl>()) {}
   LslOutlet::~LslOutlet() { close(); }
   
   bool LslOutlet::open(const QString& outletName, double nominalSampleRate) {
   #if defined(MOSAIC_HAVE_LSL)
       try {
           lsl::stream_info videoInfo(
               (outletName + "_Video").toStdString(),
               "VideoFrames",
               3,                     // camera_index, frame_id, elapsed_ns
               nominalSampleRate,
               lsl::cf_double64,
               (outletName + "_video_uid").toStdString()
           );
           videoInfo.desc().append_child("channels")
               .append_child("channel").append_child_value("label", "camera_index")
               .parent().parent()
               .append_child("channel").append_child_value("label", "frame_id")
               .parent().parent()
               .append_child("channel").append_child_value("label", "elapsed_ns");
   
           d->videoOutlet = std::make_unique<lsl::stream_outlet>(videoInfo, 360);
   
           lsl::stream_info eventInfo(
               (outletName + "_Events").toStdString(),
               "Markers",
               1,
               lsl::IRREGULAR_RATE,
               lsl::cf_string,
               (outletName + "_events_uid").toStdString()
           );
           d->eventOutlet = std::make_unique<lsl::stream_outlet>(eventInfo, 128);
   
           d->open = true;
           log_info(QString("[LSL Outlet] Opened streams: '%1_Video', '%1_Events'")
                        .arg(outletName));
           return true;
       } catch (const std::exception& e) {
           log_error(QString("[LSL Outlet] Failed to open: %1").arg(e.what()));
           return false;
       }
   #else
       (void)outletName; (void)nominalSampleRate;
       log_info("[LSL Outlet] Built without LSL support — outlet disabled.");
       return false;
   #endif
   }
   
   void LslOutlet::close() {
   #if defined(MOSAIC_HAVE_LSL)
       d->videoOutlet.reset();
       d->eventOutlet.reset();
   #endif
       d->open = false;
   }
   
   bool LslOutlet::is_open() const { return d->open; }
   
   void LslOutlet::push_frame(int cameraIndex, int64_t frameId, int64_t elapsedNs) {
   #if defined(MOSAIC_HAVE_LSL)
       if (!d->videoOutlet) { return; }
       const double sample[3] = {
           static_cast<double>(cameraIndex),
           static_cast<double>(frameId),
           static_cast<double>(elapsedNs),
       };
       d->videoOutlet->push_sample(sample);
   #else
       (void)cameraIndex; (void)frameId; (void)elapsedNs;
   #endif
   }
   
   void LslOutlet::push_event(const QString& label) {
   #if defined(MOSAIC_HAVE_LSL)
       if (!d->eventOutlet) { return; }
       const std::string s = label.toStdString();
       d->eventOutlet->push_sample(&s);
   #else
       (void)label;
   #endif
   }
   
   } // namespace mosaic
