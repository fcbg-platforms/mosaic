#include "video/video_manager.hpp"
#include "utils/ring_buffer.hpp"
#include "video/video_encoder.hpp"
#include "video/video_grabber.hpp"
#include "utils/logger.hpp"
#include <QDir>
#include <atomic>

namespace mosaic {

// Ring buffer capacity: 2 seconds worth of frames at 60 fps = 120 slots.
static constexpr std::size_t k_ring_capacity = 128;

struct CameraUnit {
    std::unique_ptr<RingBuffer<std::shared_ptr<VideoFrame>>> buffer;
    std::unique_ptr<VideoGrabber>                            grabber;
    std::unique_ptr<VideoEncoder>                            encoder;
    bool encoderDone{false};
};

struct VideoManager::Impl {
    std::vector<CameraUnit> units;
    bool                    recording{false};
    std::atomic<int>        stoppedCount{0};
    int                     cameraCount{0};

    std::atomic<int64_t>    totalEncoded{0};
    std::atomic<int64_t>    totalDropped{0};
};

VideoManager::VideoManager(QObject* parent)
    : QObject(parent), d(std::make_unique<Impl>()) {}

VideoManager::~VideoManager() { stop(); close(); }

// ── Open / Close ───────────────────────────────────────────────────────────

int VideoManager::open(const VideoSettings& settings) {
    close();

    int opened = 0;
    d->units.reserve(settings.cameras.size());

    for (int i = 0; i < static_cast<int>(settings.cameras.size()); ++i) {
        const auto& cam = settings.cameras[static_cast<size_t>(i)];

        auto unit    = CameraUnit{};
        unit.buffer  = std::make_unique<RingBuffer<std::shared_ptr<VideoFrame>>>(k_ring_capacity);
        unit.grabber = std::make_unique<VideoGrabber>(i, cam, *unit.buffer);

        connect(unit.grabber.get(), &VideoGrabber::opened,
                this,               &VideoManager::camera_opened,   Qt::QueuedConnection);
        connect(unit.grabber.get(), &VideoGrabber::closed,
                this,               &VideoManager::camera_closed,   Qt::QueuedConnection);
        connect(unit.grabber.get(), &VideoGrabber::frame_dropped,
                this,               &VideoManager::frame_dropped,   Qt::QueuedConnection);
        connect(unit.grabber.get(), &VideoGrabber::grab_error,
                this,               &VideoManager::camera_error,    Qt::QueuedConnection);

        connect(unit.grabber.get(), &VideoGrabber::frame_dropped, this,
                [this](int /*cam*/, int64_t /*id*/) {
                    d->totalDropped.fetch_add(1);
                }, Qt::QueuedConnection);

        if (unit.grabber->open()) {
            d->units.push_back(std::move(unit));
            ++opened;
        }
    }

    d->cameraCount = opened;
    log_info(QString("[VideoManager] %1 of %2 camera(s) opened.")
                 .arg(opened).arg(settings.cameras.size()));
    return opened;
}

void VideoManager::close() {
    stop();
    for (auto& unit : d->units) {
        if (unit.grabber) { unit.grabber->close(); }
    }
    d->units.clear();
    d->cameraCount = 0;
}

// ── Recording lifecycle ────────────────────────────────────────────────────

void VideoManager::start(const QString&       sessionDir,
                          const QString&       videoBasename,
                          const VideoSettings& settings) {
    if (d->recording || d->units.empty()) { return; }

    d->totalEncoded.store(0);
    d->totalDropped.store(0);
    d->stoppedCount.store(0);

    for (int i = 0; i < static_cast<int>(d->units.size()); ++i) {
        auto& unit = d->units[static_cast<size_t>(i)];
        const auto& cam = (i < static_cast<int>(settings.cameras.size()))
            ? settings.cameras[static_cast<size_t>(i)]
            : CameraParameters{};

        const QString suffix   = QString("_%1").arg(i);
        const QString videoPath = sessionDir + "/" + videoBasename + suffix + ".mp4";
        const QString tsPath    = sessionDir + "/timestamps_cam" + QString::number(i) + ".csv";

        VideoEncoder::Config cfg;
        cfg.cameraIndex   = i;
        cfg.outputPath    = videoPath;
        cfg.timestampPath = tsPath;
        cfg.codec         = settings.codec;
        cfg.preset        = settings.preset;
        cfg.bitrate       = settings.bitrate;
        cfg.crf           = settings.crf;
        cfg.width         = cam.width;
        cfg.height        = cam.height;
        cfg.fps           = cam.fps;

        unit.encoder = std::make_unique<VideoEncoder>(cfg, *unit.buffer);

        connect(unit.encoder.get(), &VideoEncoder::encoding_stopped,
                this, &VideoManager::on_encoder_stopped, Qt::QueuedConnection);
        connect(unit.encoder.get(), &VideoEncoder::encoding_error,
                this, &VideoManager::camera_error,        Qt::QueuedConnection);

        unit.encoder->start_encoding();
        unit.grabber->start_grabbing();

        log_info(QString("[VideoManager] Camera %1 recording → %2").arg(i).arg(videoPath));
    }

    d->recording = true;
}

void VideoManager::stop() {
    if (!d->recording) { return; }
    d->recording = false;

    // Signal grabbers to stop producing — encoders will drain and finish on their own.
    for (auto& unit : d->units) {
        if (unit.grabber) { unit.grabber->stop_grabbing(); }
        if (unit.encoder) { unit.encoder->stop_encoding(); }
    }
    // Wait for all threads to exit.
    for (auto& unit : d->units) {
        if (unit.grabber) { unit.grabber->wait(5000); }
        if (unit.encoder) { unit.encoder->wait(10000); }
    }
}

void VideoManager::on_encoder_stopped(int cameraIndex, int64_t frames) {
    d->totalEncoded.fetch_add(frames);
    log_info(QString("[VideoManager] Camera %1 encoder done (%2 frames).")
                 .arg(cameraIndex).arg(frames));
    const int done = d->stoppedCount.fetch_add(1) + 1;
    if (done >= d->cameraCount) {
        emit recording_stopped();
    }
}

// ── Accessors ──────────────────────────────────────────────────────────────

bool    VideoManager::is_recording()        const { return d->recording; }
int     VideoManager::camera_count()        const { return d->cameraCount; }
int64_t VideoManager::total_frames_encoded() const { return d->totalEncoded.load(); }
int64_t VideoManager::total_frames_dropped() const { return d->totalDropped.load(); }

VideoManager::CameraStats VideoManager::camera_stats(int index) const {
    CameraStats stats;
    if (index < 0 || index >= static_cast<int>(d->units.size())) { return stats; }
    const auto& unit = d->units[static_cast<size_t>(index)];
    if (unit.grabber) {
        stats.fps            = unit.grabber->current_fps();
        stats.framesGrabbed  = unit.grabber->frames_grabbed();
        stats.framesDropped  = unit.grabber->frames_dropped();
        stats.grabberRunning = unit.grabber->isRunning();
    }
    if (unit.encoder) {
        stats.framesEncoded = unit.encoder->frames_encoded();
    }
    if (unit.buffer) {
        const auto avail = unit.buffer->available_read();
        const auto cap   = unit.buffer->capacity();
        stats.ringFillPct = (cap > 0)
            ? static_cast<int>((avail * 100ULL) / cap)
            : 0;
    }
    return stats;
}

} // namespace mosaic
