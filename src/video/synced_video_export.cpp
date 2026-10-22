#include "video/synced_video_export.hpp"

#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTextStream>
#include <algorithm>
#include <array>
#include <cmath>

#if defined(MOSAIC_HAVE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace mosaic {

std::vector<RepairMapRow> read_repair_map(const QString& csvPath, QString* error) {
    auto fail = [error](const QString& why) {
        if (error) {
            *error = why;
        }
        return std::vector<RepairMapRow>{};
    };
    QFile f(csvPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return fail("cannot read " + csvPath);
    }
    QTextStream in(&f);
    const QStringList header = in.readLine().trimmed().split(',');
    const int colOut         = static_cast<int>(header.indexOf("output_frame_index"));
    const int colSrc         = static_cast<int>(header.indexOf("source_frame_index"));
    int colMissing           = static_cast<int>(header.indexOf("missing"));
    if (colMissing < 0) {
        colMissing = static_cast<int>(header.indexOf("duplicated"));
    }
    if (colOut < 0 || colMissing < 0) {
        return fail(QFileInfo(csvPath).fileName() + " is not a repair map");
    }
    if (colSrc < 0) {
        return fail(QFileInfo(csvPath).fileName() +
                    " is from an older Frame Sync without source_frame_index: run Frame Sync "
                    "again");
    }
    const int need = std::max({colOut, colSrc, colMissing});
    std::vector<RepairMapRow> rows;
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QStringList cells = line.split(',');
        if (cells.size() <= need) {
            return fail(QFileInfo(csvPath).fileName() + ": a short line");
        }
        bool okOut = false, okSrc = false;
        RepairMapRow r;
        r.outputIndex = cells[colOut].toInt(&okOut);
        r.sourceIndex = cells[colSrc].toInt(&okSrc);
        r.missing     = cells[colMissing].trimmed().compare("true", Qt::CaseInsensitive) == 0;
        if (!okOut || !okSrc || r.outputIndex != static_cast<int>(rows.size())) {
            return fail(QFileInfo(csvPath).fileName() + ": an unreadable or out-of-order line");
        }
        rows.push_back(r);
    }
    if (rows.empty()) {
        return fail(QFileInfo(csvPath).fileName() + " has no frames");
    }
    return rows;
}

#if defined(MOSAIC_HAVE_FFMPEG)

namespace {

// "MISSING" in a 5 x 7 pixel font: just the letters it needs, so the tag can
// be drawn straight into the YUV planes, with no font or GUI library.
constexpr std::array<const char*, 7> kGlyphM = {"10001", "11011", "10101", "10101",
                                                "10001", "10001", "10001"};
constexpr std::array<const char*, 7> kGlyphI = {"01110", "00100", "00100", "00100",
                                                "00100", "00100", "01110"};
constexpr std::array<const char*, 7> kGlyphS = {"01111", "10000", "10000", "01110",
                                                "00001", "00001", "11110"};
constexpr std::array<const char*, 7> kGlyphN = {"10001", "11001", "10101", "10011",
                                                "10001", "10001", "10001"};
constexpr std::array<const char*, 7> kGlyphG = {"01110", "10001", "10000", "10111",
                                                "10001", "10001", "01110"};
const std::array<const std::array<const char*, 7>*, 7> kMissing = {
    &kGlyphM, &kGlyphI, &kGlyphS, &kGlyphS, &kGlyphI, &kGlyphN, &kGlyphG};

// BT.601 limited range, as the H.264 videos are: the tag's red (220, 40, 40)
// and white.
constexpr uint8_t kRedY = 97, kRedU = 101, kRedV = 207;
constexpr uint8_t kWhiteY = 235, kGreyUV = 128;

// The red MISSING tag in the top-left corner of a YUV 4:2:0 frame, sized as
// sync_repair/marker.py drew it: a box about 1/18 of the frame's height.
void draw_missing_tag(AVFrame* f) {
    const int boxH  = std::min(std::max(14, f->height / 18), f->height);
    const int scale = std::max(1, boxH / 11);
    const int pad   = (boxH - 7 * scale) / 2;
    const int textW = 7 * 5 * scale + 6 * scale;
    const int boxW  = std::min(f->width, textW + 2 * pad);
    auto set        = [f](int x, int y, uint8_t yv, uint8_t u, uint8_t v) {
        if (x < 0 || y < 0 || x >= f->width || y >= f->height) {
            return;
        }
        f->data[0][y * f->linesize[0] + x]             = yv;
        f->data[1][(y / 2) * f->linesize[1] + (x / 2)] = u;
        f->data[2][(y / 2) * f->linesize[2] + (x / 2)] = v;
    };
    for (int y = 0; y < boxH; ++y) {
        for (int x = 0; x < boxW; ++x) {
            set(x, y, kRedY, kRedU, kRedV);
        }
    }
    for (int g = 0; g < static_cast<int>(kMissing.size()); ++g) {
        const auto& glyph = *kMissing[g];
        const int gx      = pad + g * 6 * scale;
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (glyph[row][col] != '1') {
                    continue;
                }
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        set(gx + col * scale + dx, pad + row * scale + dy, kWhiteY, kGreyUV,
                            kGreyUV);
                    }
                }
            }
        }
    }
}

// Reads a video's frames in order.
struct Decoder {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* ctx  = nullptr;
    AVPacket* pkt        = nullptr;
    int stream           = -1;
    bool flushed         = false;

    ~Decoder() {
        av_packet_free(&pkt);
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
    }

    QString open(const QString& path) {
        const QByteArray p = path.toUtf8();
        if (avformat_open_input(&fmt, p.constData(), nullptr, nullptr) < 0) {
            return "cannot open " + path;
        }
        if (avformat_find_stream_info(fmt, nullptr) < 0) {
            return "cannot read the streams of " + path;
        }
        const AVCodec* codec = nullptr;
        stream               = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (stream < 0 || !codec) {
            return "no video stream in " + path;
        }
        ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(ctx, fmt->streams[stream]->codecpar);
        ctx->thread_count = 0; // as many as there are cores
        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            return "cannot decode " + path;
        }
        pkt = av_packet_alloc();
        return {};
    }

    // The video's bitrate, bits per second: as stored, else from its size
    // and length.
    [[nodiscard]] int64_t bitrate(const QString& path) const {
        const int64_t own = fmt->streams[stream]->codecpar->bit_rate;
        if (own > 0) {
            return own;
        }
        if (fmt->bit_rate > 0) {
            return fmt->bit_rate;
        }
        const double seconds =
            fmt->duration > 0 ? static_cast<double>(fmt->duration) / AV_TIME_BASE : 0.0;
        return seconds > 0 ? static_cast<int64_t>(QFileInfo(path).size() * 8 / seconds) : 0;
    }

    // The next frame, false at the end (or on a damaged stream: the frames
    // until there are kept).
    bool next(AVFrame* f) {
        for (;;) {
            const int r = avcodec_receive_frame(ctx, f);
            if (r == 0) {
                return true;
            }
            if (r != AVERROR(EAGAIN) || flushed) {
                return false;
            }
            for (;;) {
                if (av_read_frame(fmt, pkt) < 0) {
                    avcodec_send_packet(ctx, nullptr);
                    flushed = true;
                    break;
                }
                const bool ours = pkt->stream_index == stream;
                if (ours) {
                    avcodec_send_packet(ctx, pkt); // a bad packet only costs its frame
                }
                av_packet_unref(pkt);
                if (ours) {
                    break;
                }
            }
        }
    }
};

// Writes H.264 frames to an MP4 file.
struct Encoder {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* ctx  = nullptr;
    AVStream* stream     = nullptr;
    AVPacket* pkt        = nullptr;
    QString name;
    bool headerWritten = false;

    ~Encoder() {
        av_packet_free(&pkt);
        avcodec_free_context(&ctx);
        if (fmt) {
            if (fmt->pb) {
                avio_closep(&fmt->pb);
            }
            avformat_free_context(fmt);
        }
    }

    // The same choice as the recording's encoder (VideoEncoder): NVENC at
    // the given bitrate, or libx264 at its usual quality without a GPU.
    QString open(const QString& path, int width, int height, double fps, int64_t bitrate) {
        const QByteArray p = path.toUtf8();
        if (avformat_alloc_output_context2(&fmt, nullptr, "mp4", p.constData()) < 0) {
            return "cannot create " + path;
        }
        const AVRational rate = av_d2q(fps, 100000);
        auto try_codec        = [&](const char* codecName) -> bool {
            const AVCodec* c = avcodec_find_encoder_by_name(codecName);
            if (!c) {
                return false;
            }
            AVCodecContext* e = avcodec_alloc_context3(c);
            e->width          = width;
            e->height         = height;
            e->time_base      = av_inv_q(rate);
            e->framerate      = rate;
            e->pix_fmt        = AV_PIX_FMT_YUV420P;
            e->gop_size       = std::max(1, static_cast<int>(std::lround(fps * 2)));
            if (QByteArray(codecName).contains("nvenc")) {
                e->bit_rate = std::max<int64_t>(bitrate, 1000000);
                av_opt_set(e->priv_data, "preset", "p4", 0);
            } else {
                av_opt_set_int(e->priv_data, "crf", 23, 0);
                av_opt_set(e->priv_data, "preset", "medium", 0);
            }
            if (fmt->oformat->flags & AVFMT_GLOBALHEADER) {
                e->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }
            if (avcodec_open2(e, c, nullptr) < 0) {
                avcodec_free_context(&e);
                return false;
            }
            ctx  = e;
            name = codecName;
            return true;
        };
        if (!try_codec("h264_nvenc") && !try_codec("libx264")) {
            return "no H.264 encoder (h264_nvenc or libx264) could be opened";
        }
        stream = avformat_new_stream(fmt, nullptr);
        avcodec_parameters_from_context(stream->codecpar, ctx);
        stream->time_base = ctx->time_base;
        if (avio_open(&fmt->pb, p.constData(), AVIO_FLAG_WRITE) < 0) {
            return "cannot write " + path;
        }
        if (avformat_write_header(fmt, nullptr) < 0) {
            return "cannot write the header of " + path;
        }
        headerWritten = true;
        pkt           = av_packet_alloc();
        return {};
    }

    // Sends a frame (nullptr: the end) and writes what comes out.
    bool send(const AVFrame* f) {
        if (avcodec_send_frame(ctx, f) < 0) {
            return false;
        }
        for (;;) {
            const int r = avcodec_receive_packet(ctx, pkt);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                return true;
            }
            if (r < 0) {
                return false;
            }
            // Every frame lasts one tick. Without a duration, the MP4's edit
            // list ends one frame early with B-frames (NVENC uses them): the
            // last frame was stored but dropped by every player and decoder.
            if (pkt->duration <= 0) {
                pkt->duration = 1;
            }
            av_packet_rescale_ts(pkt, ctx->time_base, stream->time_base);
            pkt->stream_index = stream->index;
            if (av_interleaved_write_frame(fmt, pkt) < 0) {
                return false;
            }
        }
    }

    bool finish() { return send(nullptr) && av_write_trailer(fmt) == 0; }
};

} // namespace

SyncedExportResult export_synced_video(const QString& sourceVideo,
                                       const std::vector<RepairMapRow>& rows, double fps,
                                       const QString& outPath,
                                       const std::function<bool(int, int)>& progress) {
    if (rows.empty() || fps <= 0) {
        SyncedExportResult none;
        none.error = "nothing to export";
        return none;
    }
    av_log_set_level(AV_LOG_ERROR);
    const QString partial =
        QFileInfo(outPath).path() + "/" + QFileInfo(outPath).completeBaseName() + ".partial.mp4";
    QFile::remove(partial);

    SyncedExportResult out;
    {
        Decoder dec;
        Encoder enc;
        AVFrame* decoded = av_frame_alloc();
        AVFrame* current = av_frame_alloc(); // the last frame shown, YUV 4:2:0
        SwsContext* sws  = nullptr;
        auto cleanup     = [&] {
            av_frame_free(&decoded);
            av_frame_free(&current);
            sws_freeContext(sws);
        };
        auto failed = [&](const QString& why) {
            cleanup();
            out.error = why;
            return false;
        };
        auto run = [&]() -> bool {
            if (const QString e = dec.open(sourceVideo); !e.isEmpty()) {
                return failed(e);
            }
            const int w = dec.ctx->width, h = dec.ctx->height;
            if (w <= 0 || h <= 0) {
                return failed("no frame size in " + sourceVideo);
            }
            if (const QString e = enc.open(partial, w, h, fps, dec.bitrate(sourceVideo));
                !e.isEmpty()) {
                return failed(e);
            }
            current->format = AV_PIX_FMT_YUV420P;
            current->width  = w;
            current->height = h;
            if (av_frame_get_buffer(current, 0) < 0) {
                return failed("out of memory");
            }
            // Black until a first frame is decoded (a camera that never
            // delivered one): every such frame is tagged.
            const ptrdiff_t lines[4] = {current->linesize[0], current->linesize[1],
                                        current->linesize[2], 0};
            av_image_fill_black(current->data, lines, AV_PIX_FMT_YUV420P, AVCOL_RANGE_MPEG, w, h);

            int decodedIndex = -1;
            bool ended       = false;
            bool haveFrame   = false;
            const int total  = static_cast<int>(rows.size());
            const int every  = std::max(1, total / 200);
            for (int k = 0; k < total; ++k) {
                const RepairMapRow& row = rows[k];
                while (!ended && decodedIndex < row.sourceIndex) {
                    if (!dec.next(decoded)) {
                        ended = true;
                        break;
                    }
                    ++decodedIndex;
                    if (decodedIndex == row.sourceIndex) {
                        sws = sws_getCachedContext(sws, decoded->width, decoded->height,
                                                   static_cast<AVPixelFormat>(decoded->format), w,
                                                   h, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr,
                                                   nullptr, nullptr);
                        if (!sws || av_frame_make_writable(current) < 0) {
                            return failed("cannot convert the frames of " + sourceVideo);
                        }
                        sws_scale(sws, decoded->data, decoded->linesize, 0, decoded->height,
                                  current->data, current->linesize);
                        haveFrame = true;
                    }
                    av_frame_unref(decoded);
                }
                // A source that ends early repeats its last frame, tagged.
                const bool tagged = row.missing || !haveFrame || decodedIndex < row.sourceIndex;

                // A fresh copy per frame: the encoder may hold on to what it
                // is given while `current` changes.
                AVFrame* f = av_frame_alloc();
                f->format  = AV_PIX_FMT_YUV420P;
                f->width   = w;
                f->height  = h;
                if (av_frame_get_buffer(f, 0) < 0 || av_frame_copy(f, current) < 0) {
                    av_frame_free(&f);
                    return failed("out of memory");
                }
                if (tagged) {
                    draw_missing_tag(f);
                    ++out.missing;
                }
                f->pts        = k;
                f->duration   = 1; // one tick, in the encoder's time base
                const bool ok = enc.send(f);
                av_frame_free(&f);
                if (!ok) {
                    return failed("encoding failed at frame " + QString::number(k));
                }
                ++out.frames;
                if (progress && (k % every == 0 || k + 1 == total) && !progress(k + 1, total)) {
                    return failed("stopped");
                }
            }
            if (!enc.finish()) {
                return failed("could not finish " + partial);
            }
            out.encoder = enc.name;
            cleanup();
            return true;
        };
        out.ok = run();
    } // decoder and encoder closed: the file can be renamed
    if (!out.ok) {
        QFile::remove(partial);
        return out;
    }
    QFile::remove(outPath);
    if (!QFile::rename(partial, outPath)) {
        QFile::remove(partial);
        out.ok    = false;
        out.error = "cannot rename " + partial + " to " + outPath;
    }
    return out;
}

#else

SyncedExportResult export_synced_video(const QString&, const std::vector<RepairMapRow>&, double,
                                       const QString&, const std::function<bool(int, int)>&) {
    SyncedExportResult res;
    res.error = "this build of MOSAIC has no FFmpeg";
    return res;
}

#endif

} // namespace mosaic
