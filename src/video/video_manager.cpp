#include "video/video_manager.hpp"
#include "trigger/trigger_manager.hpp"
#include "utils/ring_buffer.hpp"
#include "video/video_encoder.hpp"
#include "video/video_grabber.hpp"
#include "utils/logger.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QSet>
#include <atomic>

namespace mosaic {

// Ring buffer capacity: 2 seconds worth of frames at 60 fps = 120 slots.
static constexpr std::size_t k_ring_capacity = 128;

struct CameraUnit {
    std::unique_ptr<RingBuffer<std::shared_ptr<VideoFrame>>> buffer;
    std::unique_ptr<VideoGrabber>                            grabber;
    std::unique_ptr<VideoEncoder>                            encoder;
    bool encoderDone{false};
    // Position in the *configured* settings.cameras array this unit was
    // opened from — NOT necessarily this unit's position in d->units, since
    // an earlier camera that failed to open leaves a gap. VideoGrabber keeps
    // this same value as its own cameraIndex (used for the frame_preview
    // signal, and to name video_N.mp4/timestamps_camN.csv), so anything that
    // needs to re-correlate a unit back to its CameraParameters must use
    // this field rather than the unit's position in d->units.
    int configIndex{0};
};

struct VideoManager::Impl {
    std::vector<CameraUnit> units;
    bool                    recording{false};
    bool                    previewing{false};
    std::atomic<int>        stoppedCount{0};
    int                     cameraCount{0};
    TriggerManager*         triggerMgr{nullptr};  // not owned, may be null

    std::atomic<int64_t>    totalEncoded{0};
    std::atomic<int64_t>    totalDropped{0};
};

VideoManager::VideoManager(TriggerManager* triggerMgr, QObject* parent)
    : QObject(parent), d(std::make_unique<Impl>()) {
    d->triggerMgr = triggerMgr;
}

VideoManager::~VideoManager() { stop(); close(); }

// ── Open / Close ───────────────────────────────────────────────────────────

int VideoManager::open(const VideoSettings& settings) {
    close();

    // Guard against ambiguous camera identity: VideoGrabber attaches to a
    // device by serial number, falling back to Pylon's "first device found"
    // when serialNumber is empty (video_grabber.cpp). The actual collision
    // risk is between two or more empty-serial cameras (both could attach
    // to the same "first" enumerated device) or between two cameras sharing
    // an explicit serial — a single empty-serial camera alongside others
    // that are each pinned to a specific serial is not ambiguous, so it is
    // deliberately not flagged here.
    QSet<QString> seenSerials;
    QSet<QString> duplicateSerials;
    int emptySerialCount = 0;
    for (const auto& cam : settings.cameras) {
        if (cam.serialNumber.isEmpty()) { ++emptySerialCount; continue; }
        if (seenSerials.contains(cam.serialNumber)) { duplicateSerials.insert(cam.serialNumber); }
        seenSerials.insert(cam.serialNumber);
    }
    const bool ambiguousEmpty = emptySerialCount > 1;

    int opened = 0;
    d->units.reserve(settings.cameras.size());

    for (int i = 0; i < static_cast<int>(settings.cameras.size()); ++i) {
        const auto& cam = settings.cameras[static_cast<size_t>(i)];

        if (!cam.serialNumber.isEmpty() && duplicateSerials.contains(cam.serialNumber)) {
            log_error(QString("[VideoManager] open: cam %1 serial '%2' is used by more than "
                               "one configured camera — skipping to avoid two grabbers "
                               "attaching to the same physical device. Fix the serial numbers "
                               "in the Video settings.")
                          .arg(i).arg(cam.serialNumber));
            continue;
        }
        if (cam.serialNumber.isEmpty() && ambiguousEmpty) {
            log_error(QString("[VideoManager] open: cam %1 has no serial number configured, "
                               "and more than one configured camera is missing a serial — "
                               "skipping rather than risk two grabbers attaching to the same "
                               "\"first device found\". Set this camera's serial number in the "
                               "Video settings.")
                          .arg(i));
            continue;
        }

        auto unit       = CameraUnit{};
        unit.configIndex = i;
        unit.buffer  = std::make_unique<RingBuffer<std::shared_ptr<VideoFrame>>>(k_ring_capacity);
        unit.grabber = std::make_unique<VideoGrabber>(i, cam, *unit.buffer, d->triggerMgr);

        connect(unit.grabber.get(), &VideoGrabber::opened,
                this,               &VideoManager::camera_opened,   Qt::QueuedConnection);
        connect(unit.grabber.get(), &VideoGrabber::closed,
                this,               &VideoManager::camera_closed,   Qt::QueuedConnection);
        connect(unit.grabber.get(), &VideoGrabber::frame_dropped,
                this,               &VideoManager::frame_dropped,   Qt::QueuedConnection);
        connect(unit.grabber.get(), &VideoGrabber::grab_error,
                this,               &VideoManager::camera_error,    Qt::QueuedConnection);

        connect(unit.grabber.get(), &VideoGrabber::preview_frame,
                this,               &VideoManager::frame_preview,   Qt::QueuedConnection);
        connect(unit.grabber.get(), &VideoGrabber::calibration_frame_ready,
                this,               &VideoManager::calibration_frame_ready, Qt::QueuedConnection);

        connect(unit.grabber.get(), &VideoGrabber::frame_dropped, this,
                [this](int /*cam*/, int64_t /*id*/) {
                    d->totalDropped.fetch_add(1);
                }, Qt::QueuedConnection);

        log_info(QString("[VideoManager] open: calling open() for cam %1 (serial %2)")
                     .arg(i).arg(cam.serialNumber));
        if (unit.grabber->open()) {
            log_info(QString("[VideoManager] open: cam %1 open() returned, pushing unit").arg(i));
            d->units.push_back(std::move(unit));
            log_info(QString("[VideoManager] open: cam %1 unit pushed, opened=%2").arg(i).arg(opened + 1));
            ++opened;
        } else {
            log_warning(QString("[VideoManager] open: cam %1 open() failed").arg(i));
        }
    }

    d->cameraCount = opened;
    log_info(QString("[VideoManager] %1 of %2 camera(s) opened.")
                 .arg(opened).arg(settings.cameras.size()));
    return opened;
}

void VideoManager::close() {
    log_info(QString("[VideoManager] close() units=%1 rec=%2 prev=%3")
                 .arg(d->units.size()).arg(d->recording).arg(d->previewing));
    if (d->recording) {
        stop();
    } else {
        d->previewing = false;
        for (int i = 0; i < static_cast<int>(d->units.size()); ++i) {
            auto& unit = d->units[static_cast<size_t>(i)];
            if (unit.grabber) {
                log_info(QString("[VideoManager] close: stop_grabbing cam %1").arg(i));
                unit.grabber->stop_grabbing();
                log_info(QString("[VideoManager] close: cam %1 thread stopped").arg(i));
            }
        }
    }
    log_info("[VideoManager] close: closing Pylon devices");
    for (auto& unit : d->units) {
        if (unit.grabber) { unit.grabber->close(); }
    }
    // Flush any queued MetaCallEvents (preview_frame, closed) that were posted
    // to this object by the now-stopped grabbers.  Removing them before
    // destroying the grabber objects prevents Qt from delivering events that
    // reference sender objects that are about to become dangling.
    QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
    log_info("[VideoManager] close: clearing units");
    d->units.clear();
    d->cameraCount = 0;
    log_info("[VideoManager] close: done");
}

// ── Recording lifecycle ────────────────────────────────────────────────────

void VideoManager::start_preview() {
    if (d->units.empty()) { return; }
    for (auto& unit : d->units) {
        if (unit.grabber && !unit.grabber->isRunning()) {
            unit.grabber->start_grabbing();
        }
    }
    d->previewing = true;
    log_info(QString("[VideoManager] Preview started for %1 camera(s).").arg(d->units.size()));
}

void VideoManager::start(const QString&       sessionDir,
                          const QString&       videoBasename,
                          const VideoSettings& settings) {
    if (d->recording || d->units.empty()) { return; }

    // Stop preview grabbers so ring buffers can be safely reset before recording.
    if (d->previewing) {
        for (auto& unit : d->units) {
            if (unit.grabber && unit.grabber->isRunning()) {
                unit.grabber->stop_grabbing();
                unit.grabber->wait(5000);
            }
        }
        d->previewing = false;
    }

    d->totalEncoded.store(0);
    d->totalDropped.store(0);
    d->stoppedCount.store(0);

    // Pass 1: prepare every camera's encoder (ring buffer reset, encoder
    // construction, start_encoding()) without starting any grabber yet.
    // Pass 2 then starts all 6 grabbers back-to-back, so the first-frame
    // time of each camera is not skewed by the setup cost of the others —
    // that skew previously accumulated when prep and grab-start were
    // interleaved camera-by-camera in a single loop.
    for (auto& unit : d->units) {
        // Use this unit's original config-array position, not its position
        // in d->units — those diverge as soon as any earlier camera fails
        // to open, and pairing positionally here would silently apply the
        // wrong camera's width/height/fps and write to the wrong filename.
        const int i = unit.configIndex;
        const auto& cam = (i < static_cast<int>(settings.cameras.size()))
            ? settings.cameras[static_cast<size_t>(i)]
            : CameraParameters{};

        const QString suffix   = QString("_%1").arg(i);
        const QString videoPath = sessionDir + "/" + videoBasename + suffix + ".mp4";
        const QString tsPath    = sessionDir + "/timestamps_cam" + QString::number(i) + ".csv";

        // Clear stale preview frames so the encoder starts from a clean buffer.
        unit.buffer->reset(k_ring_capacity);

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

        log_info(QString("[VideoManager] Camera %1 recording → %2").arg(i).arg(videoPath));
    }

    for (auto& unit : d->units) {
        unit.grabber->start_grabbing();
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

void VideoManager::apply_live_params(int configIndex) {
    for (auto& unit : d->units) {
        if (unit.configIndex == configIndex && unit.grabber) {
            unit.grabber->apply_live_params();
            return;
        }
    }
}

void VideoManager::request_calibration_frame(int configIndex, uint64_t token) {
    for (auto& unit : d->units) {
        if (unit.configIndex == configIndex && unit.grabber) {
            unit.grabber->request_calibration_frame(token);
            return;
        }
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
        stats.fps                = unit.grabber->current_fps();
        stats.framesGrabbed      = unit.grabber->frames_grabbed();
        stats.framesDropped      = unit.grabber->frames_dropped();
        stats.grabberRunning     = unit.grabber->isRunning();
        stats.lastFrameElapsedNs = unit.grabber->last_frame_elapsed_ns();
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
