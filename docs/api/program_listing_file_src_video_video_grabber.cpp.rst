
.. _program_listing_file_src_video_video_grabber.cpp:

Program Listing for File video_grabber.cpp
==========================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_video_video_grabber.cpp>` (``src/video/video_grabber.cpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #include "video/video_grabber.hpp"
   #include "utils/logger.hpp"
   #include "utils/timestamp.hpp"
   #include <QThread>
   #include <atomic>
   #include <chrono>
   
   #if defined(MOSAIC_HAVE_CAMERAS)
   #  include <pylon/PylonIncludes.h>
   #endif
   
   namespace mosaic {
   
   struct VideoGrabber::Impl {
       int                                      cameraIndex;
       const CameraParameters&                  params;
       RingBuffer<std::shared_ptr<VideoFrame>>& frameBuffer;
   
       std::atomic<bool>    deviceOpen   {false};
       std::atomic<int64_t> frameCounter {0};
       std::atomic<int64_t> dropCounter  {0};
       std::atomic<double>  currentFps   {0.0};
   
   #if defined(MOSAIC_HAVE_CAMERAS)
       Pylon::CInstantCamera camera;
   #endif
   
       explicit Impl(int idx,
                     const CameraParameters& p,
                     RingBuffer<std::shared_ptr<VideoFrame>>& buf)
           : cameraIndex(idx), params(p), frameBuffer(buf) {}
   };
   
   VideoGrabber::VideoGrabber(int                                      cameraIndex,
                               const CameraParameters&                  params,
                               RingBuffer<std::shared_ptr<VideoFrame>>& frameBuffer,
                               QObject*                                 parent)
       : QThread(parent)
       , d(std::make_unique<Impl>(cameraIndex, params, frameBuffer))
   {}
   
   VideoGrabber::~VideoGrabber() { stop_grabbing(); close(); }
   
   // ── Device management ──────────────────────────────────────────────────────
   
   bool VideoGrabber::open() {
       if (d->deviceOpen.load()) {
           return true;
       }
   
   #if defined(MOSAIC_HAVE_CAMERAS)
       try {
           Pylon::PylonInitialize();
   
           if (d->params.serialNumber.isEmpty()) {
               d->camera.Attach(Pylon::CTlFactory::GetInstance().CreateFirstDevice());
           } else {
               Pylon::CDeviceInfo info;
               info.SetSerialNumber(d->params.serialNumber.toStdString().c_str());
               d->camera.Attach(Pylon::CTlFactory::GetInstance().CreateDevice(info));
           }
   
           d->camera.Open();
   
           auto& cam = d->camera.GetNodeMap();
           using Pylon::CIntegerParameter;
           using Pylon::CFloatParameter;
           using Pylon::CEnumerationParameter;
           using Pylon::CBooleanParameter;
   
           CIntegerParameter(cam, "Width").SetValue(d->params.width);
           CIntegerParameter(cam, "Height").SetValue(d->params.height);
           CIntegerParameter(cam, "OffsetX").SetValue(d->params.offsetX);
           CIntegerParameter(cam, "OffsetY").SetValue(d->params.offsetY);
           CBooleanParameter(cam, "ReverseX").SetValue(d->params.reverseX);
           CBooleanParameter(cam, "ReverseY").SetValue(d->params.reverseY);
           CEnumerationParameter(cam, "PixelFormat").SetValue(
               d->params.pixelFormat.toStdString().c_str());
   
           if (d->params.specifyFps) {
               CEnumerationParameter(cam, "AcquisitionFrameRateEnable").SetValue("true");
               CFloatParameter(cam, "AcquisitionFrameRate").SetValue(d->params.fps);
           }
   
           CEnumerationParameter(cam, "ExposureAuto").SetValue(
               d->params.exposureAuto.toStdString().c_str());
           if (d->params.exposureAuto == "Off") {
               CFloatParameter(cam, "ExposureTime").SetValue(d->params.exposureTimeUs);
           }
   
           CEnumerationParameter(cam, "GainAuto").SetValue(
               d->params.gainAuto.toStdString().c_str());
           if (d->params.gainAuto == "Off") {
               CFloatParameter(cam, "Gain").SetValue(d->params.gainDb);
           }
   
           CFloatParameter(cam, "Gamma").SetValue(d->params.gamma);
   
           d->deviceOpen.store(true);
   
           const int    w   = static_cast<int>(CIntegerParameter(cam, "Width").GetValue());
           const int    h   = static_cast<int>(CIntegerParameter(cam, "Height").GetValue());
           const double fps = d->params.specifyFps
               ? d->params.fps
               : CFloatParameter(cam, "ResultingFrameRate").GetValue();
   
           log_info(QString("[Camera %1] Opened: %2×%3 @ %4 fps (serial: %5)")
                        .arg(d->cameraIndex).arg(w).arg(h).arg(fps)
                        .arg(d->params.serialNumber));
   
           emit opened(d->cameraIndex, w, h, fps);
           return true;
   
       } catch (const Pylon::GenericException& e) {
           log_error(QString("[Camera %1] Open failed: %2")
                         .arg(d->cameraIndex)
                         .arg(QString::fromStdString(e.GetDescription())));
           return false;
       }
   #else
       // Stub: always succeeds.
       d->deviceOpen.store(true);
       log_info(QString("[Camera %1] Stub opened (%2×%3 @ %4 fps)")
                    .arg(d->cameraIndex)
                    .arg(d->params.width)
                    .arg(d->params.height)
                    .arg(d->params.fps));
       emit opened(d->cameraIndex, d->params.width, d->params.height, d->params.fps);
       return true;
   #endif
   }
   
   void VideoGrabber::close() {
       if (!d->deviceOpen.load()) { return; }
   
   #if defined(MOSAIC_HAVE_CAMERAS)
       try {
           if (d->camera.IsOpen()) {
               d->camera.StopGrabbing();
               d->camera.Close();
           }
       } catch (...) {}
   #endif
   
       d->deviceOpen.store(false);
       emit closed(d->cameraIndex);
   }
   
   // ── Grab control ───────────────────────────────────────────────────────────
   
   void VideoGrabber::start_grabbing() {
       if (!d->deviceOpen.load()) { return; }
       d->frameCounter.store(0);
       d->dropCounter.store(0);
       QThread::start();
   }
   
   void VideoGrabber::stop_grabbing() {
       requestInterruption();
       wait(5000);
   }
   
   // ── Run loop ───────────────────────────────────────────────────────────────
   
   void VideoGrabber::run() {
   #if defined(MOSAIC_HAVE_CAMERAS)
       run_pylon_loop();
   #else
       run_stub_loop();
   #endif
   }
   
   #if defined(MOSAIC_HAVE_CAMERAS)
   void VideoGrabber::run_pylon_loop() {
       try {
           d->camera.StartGrabbing(Pylon::GrabStrategy_LatestImageOnly,
                                   Pylon::GrabLoop_ProvidedByUser);
   
           Pylon::CGrabResultPtr result;
           auto lastFpsTime  = SteadyClock::now();
           int64_t fpsCount  = 0;
   
           while (!isInterruptionRequested()) {
               if (!d->camera.RetrieveResult(100, result, Pylon::TimeoutHandling_Return)) {
                   continue;
               }
               if (!result->GrabSucceeded()) {
                   log_warning(QString("[Camera %1] Grab failed: %2")
                                   .arg(d->cameraIndex)
                                   .arg(QString::fromStdString(result->GetErrorDescription())));
                   continue;
               }
   
               const int64_t frameId = d->frameCounter.fetch_add(1) + 1;
               const int64_t tsNs    = elapsed_ns();
               const int64_t wallNs  = wall_clock_ns();
   
               const int w = static_cast<int>(result->GetWidth());
               const int h = static_cast<int>(result->GetHeight());
   
               auto frame       = std::make_shared<VideoFrame>();
               frame->cameraIndex = d->cameraIndex;
               frame->frameId     = frameId;
               frame->elapsedNs   = tsNs;
               frame->wallClockNs = wallNs;
               frame->width       = w;
               frame->height      = h;
               frame->stride      = w * 3;
               frame->data.resize(static_cast<size_t>(w * h * 3));
   
               std::memcpy(frame->data.data(),
                           result->GetBuffer(),
                           frame->data.size());
   
               if (!d->frameBuffer.push(std::move(frame))) {
                   d->dropCounter.fetch_add(1);
                   emit frame_dropped(d->cameraIndex, frameId);
               }
   
               // Rolling FPS estimate over 1-second windows.
               ++fpsCount;
               const auto now = SteadyClock::now();
               const auto dt  = std::chrono::duration<double>(now - lastFpsTime).count();
               if (dt >= 1.0) {
                   d->currentFps.store(static_cast<double>(fpsCount) / dt);
                   fpsCount   = 0;
                   lastFpsTime = now;
               }
           }
   
           d->camera.StopGrabbing();
   
       } catch (const Pylon::GenericException& e) {
           emit grab_error(d->cameraIndex,
                           QString::fromStdString(e.GetDescription()));
       }
   }
   #else
   void VideoGrabber::run_pylon_loop() { run_stub_loop(); }
   #endif
   
   void VideoGrabber::run_stub_loop() {
       const double fps   = d->params.fps > 0.0 ? d->params.fps : 30.0;
       const int    width  = d->params.width  > 0 ? d->params.width  : 1920;
       const int    height = d->params.height > 0 ? d->params.height : 1080;
       // Convert floating-point period to the steady_clock's native duration type.
       const SteadyClock::duration period =
           std::chrono::duration_cast<SteadyClock::duration>(
               std::chrono::duration<double>(1.0 / fps));
   
       // Pre-allocate a reusable test-pattern buffer (BGR colour bars).
       std::vector<uint8_t> pattern(static_cast<size_t>(width * height * 3));
       {
           // Seven ITU colour bars: white, yellow, cyan, green, magenta, red, blue.
           // Stored as {B, G, R} to match BGR8 layout.
           const std::array<std::array<uint8_t, 3>, 7> bars = {{
               {255,255,255}, {  0,255,255}, {255,255,  0},
               {  0,255,  0}, {255,  0,255}, {  0,  0,255}, {  0,  0,255}
           }};
           const int barW = width / 7;
           for (int row = 0; row < height; ++row) {
               for (int col = 0; col < width; ++col) {
                   const int bar = std::min(col / barW, 6);
                   const std::size_t offset = static_cast<std::size_t>((row * width + col) * 3);
                   pattern[offset + 0] = bars[static_cast<std::size_t>(bar)][0];
                   pattern[offset + 1] = bars[static_cast<std::size_t>(bar)][1];
                   pattern[offset + 2] = bars[static_cast<std::size_t>(bar)][2];
               }
           }
       }
   
       auto nextFrame   = SteadyClock::now();
       auto lastFpsTime = nextFrame;
       int64_t fpsCount = 0;
   
       while (!isInterruptionRequested()) {
           const auto now = SteadyClock::now();
           if (now < nextFrame) {
               QThread::msleep(1);
               continue;
           }
           nextFrame += period;
   
           const int64_t frameId = d->frameCounter.fetch_add(1) + 1;
   
           // Embed frame counter as a white rectangle in top-left so we can
           // verify frame ordering during testing.
           auto frame         = std::make_shared<VideoFrame>();
           frame->cameraIndex = d->cameraIndex;
           frame->frameId     = frameId;
           frame->elapsedNs   = elapsed_ns();
           frame->wallClockNs = wall_clock_ns();
           frame->width       = width;
           frame->height      = height;
           frame->stride      = width * 3;
           frame->data        = pattern;   // copy of pre-built pattern
   
           // Stamp a small white rectangle top-left so frame ordering is visible.
           const int stampH = std::min(20, height);
           const int stampW = std::min(80, width);
           for (int row = 0; row < stampH; ++row) {
               for (int col = 0; col < stampW; ++col) {
                   const std::size_t off = static_cast<std::size_t>((row * width + col) * 3);
                   frame->data[off] = frame->data[off + 1] = frame->data[off + 2] = 255;
               }
           }
   
           if (!d->frameBuffer.push(std::move(frame))) {
               d->dropCounter.fetch_add(1);
               emit frame_dropped(d->cameraIndex, frameId);
           }
   
           ++fpsCount;
           const double elapsed = std::chrono::duration<double>(now - lastFpsTime).count();
           if (elapsed >= 1.0) {
               d->currentFps.store(static_cast<double>(fpsCount) / elapsed);
               fpsCount    = 0;
               lastFpsTime = now;
           }
       }
   }
   
   // ── Accessors ──────────────────────────────────────────────────────────────
   
   bool    VideoGrabber::is_open()        const { return d->deviceOpen.load(); }
   int64_t VideoGrabber::frames_grabbed() const { return d->frameCounter.load(); }
   int64_t VideoGrabber::frames_dropped() const { return d->dropCounter.load(); }
   double  VideoGrabber::current_fps()    const { return d->currentFps.load(); }
   
   } // namespace mosaic
