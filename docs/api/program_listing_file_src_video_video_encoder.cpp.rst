
.. _program_listing_file_src_video_video_encoder.cpp:

Program Listing for File video_encoder.cpp
==========================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_video_video_encoder.cpp>` (``src\video\video_encoder.cpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #include "video/video_encoder.hpp"
   #include "utils/logger.hpp"
   #include "utils/timestamp.hpp"
   #include <QFile>
   #include <atomic>
   #include <chrono>
   
   #if defined(MOSAIC_HAVE_FFMPEG)
   extern "C" {
   #  include <libavcodec/avcodec.h>
   #  include <libavformat/avformat.h>
   #  include <libavutil/avutil.h>
   #  include <libavutil/opt.h>
   #  include <libswscale/swscale.h>
   }
   #endif
   
   namespace mosaic {
   
   struct VideoEncoder::Impl {
       Config                                   cfg;
       RingBuffer<std::shared_ptr<VideoFrame>>& frameBuffer;
       FrameTimestampWriter                     tsWriter;
   
       std::atomic<bool>    stopFlag    {false};
       std::atomic<int64_t> encCount    {0};
       std::atomic<int64_t> dropCount   {0};
   
       explicit Impl(const Config& c,
                     RingBuffer<std::shared_ptr<VideoFrame>>& buf)
           : cfg(c), frameBuffer(buf) {}
   };
   
   VideoEncoder::VideoEncoder(const Config&                           cfg,
                               RingBuffer<std::shared_ptr<VideoFrame>>& frameBuffer,
                               QObject*                                parent)
       : QThread(parent)
       , d(std::make_unique<Impl>(cfg, frameBuffer))
   {}
   
   VideoEncoder::~VideoEncoder() { stop_encoding(); wait(5000); }
   
   void VideoEncoder::start_encoding() {
       d->stopFlag.store(false);
       d->encCount.store(0);
       d->dropCount.store(0);
       QThread::start();
   }
   
   void VideoEncoder::stop_encoding() { d->stopFlag.store(true); }
   
   int64_t VideoEncoder::frames_encoded() const { return d->encCount.load(); }
   int64_t VideoEncoder::frames_dropped() const { return d->dropCount.load(); }
   
   // ── Run ────────────────────────────────────────────────────────────────────
   
   void VideoEncoder::run() {
   #if defined(MOSAIC_HAVE_FFMPEG)
       run_ffmpeg_loop();
   #else
       run_stub_loop();
   #endif
   }
   
   // ── Stub fallback (no FFmpeg) ──────────────────────────────────────────────
   
   void VideoEncoder::run_stub_loop() {
       log_warning(QString("[Encoder %1] FFmpeg not available — frames discarded.")
                       .arg(d->cfg.cameraIndex));
   
       if (!d->tsWriter.start(d->cfg.timestampPath)) {
           emit encoding_error(d->cfg.cameraIndex,
                               QString("Cannot open timestamp file: %1").arg(d->cfg.timestampPath));
           return;
       }
   
       emit encoding_started(d->cfg.cameraIndex);
   
       while (!d->stopFlag.load()) {
           std::shared_ptr<VideoFrame> frame;
           if (!d->frameBuffer.pop(frame)) {
               QThread::msleep(1);
               continue;
           }
           d->tsWriter.write(frame->frameId, frame->elapsedNs, frame->wallClockNs, frame->hwTimestampNs);
           d->encCount.fetch_add(1);
       }
   
       // Drain remaining frames.
       std::shared_ptr<VideoFrame> frame;
       while (d->frameBuffer.pop(frame)) {
           d->tsWriter.write(frame->frameId, frame->elapsedNs, frame->wallClockNs, frame->hwTimestampNs);
           d->encCount.fetch_add(1);
       }
   
       d->tsWriter.stop();
       emit encoding_stopped(d->cfg.cameraIndex, d->encCount.load());
   }
   
   // ── FFmpeg encode loop ─────────────────────────────────────────────────────
   
   #if defined(MOSAIC_HAVE_FFMPEG)
   
   static void ffmpeg_log_callback(void*, int level, const char* fmt, va_list args) {
       if (level > AV_LOG_WARNING) { return; }
       char buf[512];
       std::vsnprintf(buf, sizeof(buf), fmt, args);
       // Strip trailing newline
       int len = static_cast<int>(std::strlen(buf));
       while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) { buf[--len] = '\0'; }
       if (level <= AV_LOG_ERROR) { log_error(QString("[FFmpeg] %1").arg(buf)); }
       else { log_warning(QString("[FFmpeg] %1").arg(buf)); }
   }
   
   void VideoEncoder::run_ffmpeg_loop() {
       av_log_set_callback(ffmpeg_log_callback);
   
       const auto& cfg = d->cfg;
   
       // ── Codec selection: preferred → libx264 fallback ───────────────────────
       // Keep QByteArrays alive for the duration of the calls that use their data.
       const QByteArray preferredNameBytes = cfg.codec.toUtf8();
       const AVCodec* codec = avcodec_find_encoder_by_name(preferredNameBytes.constData());
       bool usingGpu = cfg.codec.contains("nvenc") || cfg.codec.contains("videotoolbox");
       if (!codec) {
           log_warning(QString("[Encoder %1] Codec '%2' not found, falling back to libx264")
                           .arg(cfg.cameraIndex).arg(cfg.codec));
           codec   = avcodec_find_encoder_by_name("libx264");
           usingGpu = false;
       }
       if (!codec) {
           emit encoding_error(cfg.cameraIndex, "No suitable video codec found (tried libx264).");
           return;
       }
   
       // ── Open output format context ──────────────────────────────────────────
       const QByteArray outputPathBytes = cfg.outputPath.toUtf8();
       AVFormatContext* fmtCtx = nullptr;
       if (avformat_alloc_output_context2(&fmtCtx, nullptr, nullptr,
                                           outputPathBytes.constData()) < 0) {
           emit encoding_error(cfg.cameraIndex,
                               QString("Cannot create output context for: %1").arg(cfg.outputPath));
           return;
       }
   
       AVStream* stream = avformat_new_stream(fmtCtx, nullptr);
       if (!stream) {
           avformat_free_context(fmtCtx);
           emit encoding_error(cfg.cameraIndex, "Cannot allocate output stream.");
           return;
       }
   
       // ── Build a codec context for the given codec ───────────────────────────
       // Returns nullptr if avcodec_open2 fails (caller should retry with fallback).
       auto try_open_codec = [&](const AVCodec* c, bool gpu) -> AVCodecContext* {
           AVCodecContext* ctx = avcodec_alloc_context3(c);
           if (!ctx) { return nullptr; }
   
           ctx->width     = cfg.width;
           ctx->height    = cfg.height;
           // 1 ms time_base lets PTS track real elapsed time from elapsed_ns.
           // Frame rate hint uses the nominal fps; actual inter-frame spacing
           // is determined by each frame's measured elapsed_ns timestamp.
           ctx->time_base = AVRational{1, 1000};
           ctx->framerate = AVRational{static_cast<int>(std::round(cfg.fps)), 1};
           ctx->pix_fmt   = AV_PIX_FMT_YUV420P;
           ctx->gop_size  = static_cast<int>(cfg.fps * 2);
   
           if (gpu) {
               ctx->bit_rate = cfg.bitrate * 1000LL;
               const QByteArray preset = cfg.preset.toUtf8();
               av_opt_set(ctx->priv_data, "preset", preset.constData(), 0);
           } else {
               av_opt_set_int(ctx->priv_data, "crf", cfg.crf, 0);
               av_opt_set(ctx->priv_data, "preset", "medium", 0);
           }
   
           if (fmtCtx->oformat->flags & AVFMT_GLOBALHEADER) {
               ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
           }
   
           if (avcodec_open2(ctx, c, nullptr) != 0) {
               avcodec_free_context(&ctx);
               return nullptr;
           }
           return ctx;
       };
   
       // ── Open codec; fall back to libx264 if the GPU encoder fails at runtime ─
       AVCodecContext* ctx = try_open_codec(codec, usingGpu);
       if (!ctx && usingGpu) {
           log_warning(QString("[Encoder %1] GPU codec '%2' failed to open "
                               "(no compatible GPU/driver?), falling back to libx264")
                           .arg(cfg.cameraIndex).arg(cfg.codec));
           const AVCodec* sw = avcodec_find_encoder_by_name("libx264");
           if (sw) {
               codec    = sw;
               usingGpu = false;
               ctx      = try_open_codec(codec, false);
           }
       }
       if (!ctx) {
           avformat_free_context(fmtCtx);
           emit encoding_error(cfg.cameraIndex,
                               QString("Cannot open codec '%1' or libx264 fallback.").arg(cfg.codec));
           return;
       }
   
       avcodec_parameters_from_context(stream->codecpar, ctx);
       stream->time_base = ctx->time_base;
   
       // ── Open file for writing ───────────────────────────────────────────────
       if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
           if (avio_open(&fmtCtx->pb, outputPathBytes.constData(), AVIO_FLAG_WRITE) < 0) {
               avcodec_free_context(&ctx);
               avformat_free_context(fmtCtx);
               emit encoding_error(cfg.cameraIndex,
                                   QString("Cannot open output file: %1").arg(cfg.outputPath));
               return;
           }
       }
   
       if (avformat_write_header(fmtCtx, nullptr) < 0) {
           if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) { avio_closep(&fmtCtx->pb); }
           avcodec_free_context(&ctx);
           avformat_free_context(fmtCtx);
           emit encoding_error(cfg.cameraIndex,
                               QString("Cannot write header for: %1").arg(cfg.outputPath));
           return;
       }
   
       // ── Colour conversion context (BGR24 → YUV420P) ─────────────────────────
       SwsContext* swsCtx = sws_getContext(
           cfg.width, cfg.height, AV_PIX_FMT_BGR24,
           cfg.width, cfg.height, AV_PIX_FMT_YUV420P,
           SWS_BILINEAR, nullptr, nullptr, nullptr);
       if (!swsCtx) {
           av_write_trailer(fmtCtx);
           avcodec_free_context(&ctx);
           avformat_free_context(fmtCtx);
           emit encoding_error(cfg.cameraIndex, "Cannot create SwsContext.");
           return;
       }
   
       // ── Allocate reusable AVFrame (YUV destination) ─────────────────────────
       AVFrame* avFrame = av_frame_alloc();
       avFrame->format = AV_PIX_FMT_YUV420P;
       avFrame->width  = cfg.width;
       avFrame->height = cfg.height;
       av_frame_get_buffer(avFrame, 32);
   
       AVPacket* pkt = av_packet_alloc();
   
       if (!d->tsWriter.start(cfg.timestampPath)) {
           log_warning(QString("[Encoder %1] Cannot open timestamp file — continuing without it.")
                           .arg(cfg.cameraIndex));
       }
   
       emit encoding_started(cfg.cameraIndex);
   
       // PTS is derived from each frame's elapsed_ns (steady_clock at grab time).
       // Subtracting the first frame's timestamp gives t=0 at recording start.
       // time_base = 1/1000, so: pts = elapsed_ms = elapsed_ns / 1_000_000.
       int64_t startNs = -1;
   
       auto encode_frame = [&](AVFrame* frame) {
           if (avcodec_send_frame(ctx, frame) < 0) { return; }
           while (true) {
               const int ret = avcodec_receive_packet(ctx, pkt);
               if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) { break; }
               if (ret < 0) { break; }
               av_packet_rescale_ts(pkt, ctx->time_base, stream->time_base);
               pkt->stream_index = stream->index;
               av_interleaved_write_frame(fmtCtx, pkt);
               av_packet_unref(pkt);
           }
       };
   
       // ── Main encode loop ───────────────────────────────────────────────────
       while (!d->stopFlag.load()) {
           std::shared_ptr<VideoFrame> frame;
           if (!d->frameBuffer.pop(frame)) {
               QThread::msleep(1);
               continue;
           }
           if (!frame || !frame->is_valid()) { continue; }
   
           // Colour-convert BGR → YUV420P
           av_frame_make_writable(avFrame);
           const uint8_t* srcData[1] = { frame->data.data() };
           const int      srcStride[1] = { frame->stride };
           sws_scale(swsCtx,
                     srcData, srcStride, 0, cfg.height,
                     avFrame->data, avFrame->linesize);
   
           if (startNs < 0) startNs = frame->elapsedNs;
           avFrame->pts = (frame->elapsedNs - startNs) / 1'000'000;  // ns → ms
           encode_frame(avFrame);
   
           d->tsWriter.write(frame->frameId, frame->elapsedNs, frame->wallClockNs, frame->hwTimestampNs);
           d->encCount.fetch_add(1);
       }
   
       // Drain remaining buffered frames.
       {
           std::shared_ptr<VideoFrame> frame;
           while (d->frameBuffer.pop(frame)) {
               if (!frame || !frame->is_valid()) { continue; }
               av_frame_make_writable(avFrame);
               const uint8_t* srcData[1] = { frame->data.data() };
               const int      srcStride[1] = { frame->stride };
               sws_scale(swsCtx, srcData, srcStride, 0, cfg.height,
                         avFrame->data, avFrame->linesize);
               if (startNs < 0) startNs = frame->elapsedNs;
               avFrame->pts = (frame->elapsedNs - startNs) / 1'000'000;
               encode_frame(avFrame);
               d->tsWriter.write(frame->frameId, frame->elapsedNs, frame->wallClockNs, frame->hwTimestampNs);
               d->encCount.fetch_add(1);
           }
       }
   
       // Flush encoder.
       encode_frame(nullptr);
   
       av_write_trailer(fmtCtx);
   
       // ── Cleanup ─────────────────────────────────────────────────────────────
       sws_freeContext(swsCtx);
       av_frame_free(&avFrame);
       av_packet_free(&pkt);
       avcodec_free_context(&ctx);
       if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
           avio_closep(&fmtCtx->pb);
       }
       avformat_free_context(fmtCtx);
   
       d->tsWriter.stop();
   
       log_info(QString("[Encoder %1] Finished. %2 frames encoded.")
                    .arg(cfg.cameraIndex).arg(d->encCount.load()));
       emit encoding_stopped(cfg.cameraIndex, d->encCount.load());
   }
   
   #endif // MOSAIC_HAVE_FFMPEG
   
   } // namespace mosaic
