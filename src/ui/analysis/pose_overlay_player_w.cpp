#include "ui/analysis/pose_overlay_player_w.hpp"
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QStackedLayout>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>
#include <cmath>
#include <functional>

namespace mosaic {

namespace {

QString format_ms(int64_t ms) {
    const int64_t totalSec = ms / 1000;
    return QString("%1:%2")
        .arg(totalSec / 60, 2, 10, QChar('0'))
        .arg(totalSec % 60, 2, 10, QChar('0'));
}

// Per-track-id color palette for the 3D-pose-reconstruction overlay — same
// shape as GazeRoomViewW's kCameraColors, distinct hues so 2+ simultaneous
// tracked people are visually separable.
const QColor kTrackColors[] = {
    QColor("#00dcff"), QColor("#ffdd44"), QColor("#ff4488"),
    QColor("#44cc44"), QColor("#cc44cc"), QColor("#ff8844"),
};

} // namespace

// ── SkeletonOverlayW — transparent widget drawing the current pose frame ──

class SkeletonOverlayW : public QWidget {
public:
    explicit SkeletonOverlayW(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setStyleSheet("background:transparent;");
    }

    // update() matters here, not just bookkeeping: set_frame()/etc. are
    // typically called before the video's first decoded frame arrives (size
    // still unknown), so paintEvent() bails out with nothing drawn. Without
    // this repaint, the overlay stays blank until something else (e.g.
    // playback) calls set_frame() again — a user who loads a camera and
    // looks at the paused first frame would see no markers at all.
    void set_native_size(QSize size) { nativeSize_ = size; update(); }

    void set_frame(const PoseFrame* frame, QVector<QPair<int, int>> skeletonEdges) {
        frame_          = frame;
        skeletonEdges_  = std::move(skeletonEdges);
        exprFrame_      = nullptr;   // mutually exclusive with expression/gaze/skeleton3d mode
        gazeFrame_      = nullptr;
        skeleton3dFrame_ = nullptr;
        update();
    }

    // Facial-expression draw mode — mutually exclusive with set_frame()/
    // set_gaze_frame()/set_skeleton3d_frame() (see
    // PoseOverlayPlayerW::set_pose_result()/set_expression_result()/
    // set_gaze_result()/set_skeleton3d_result() for why every setter clears
    // the others).
    void set_expression_frame(const ExpressionFrame* frame) {
        exprFrame_     = frame;
        frame_         = nullptr;
        gazeFrame_     = nullptr;
        skeleton3dFrame_ = nullptr;
        skeletonEdges_.clear();
        update();
    }

    // Gaze-fusion draw mode — mutually exclusive with set_frame()/
    // set_expression_frame()/set_skeleton3d_frame() (see above). cameraIndex
    // picks which GazeFusionFrame::perCamera entry to draw (the
    // currently-loaded video's own camera).
    void set_gaze_frame(const GazeFusionFrame* frame, int cameraIndex) {
        gazeFrame_       = frame;
        gazeCameraIndex_ = cameraIndex;
        frame_           = nullptr;
        exprFrame_       = nullptr;
        skeleton3dFrame_ = nullptr;
        skeletonEdges_.clear();
        update();
    }

    // 3D-pose-reconstruction draw mode — mutually exclusive with
    // set_frame()/set_expression_frame()/set_gaze_frame() (see above).
    // cameraIndex picks which entry of each person's reprojectedPx map
    // applies (the currently-loaded video's own camera); skeletonEdges is
    // the result's own edge list (drawn purely from precomputed pixel
    // coordinates — zero calibration math here).
    void set_skeleton3d_frame(const Skeleton3DFrame* frame, int cameraIndex,
                               QVector<QPair<int, int>> skeletonEdges) {
        skeleton3dFrame_       = frame;
        skeleton3dCameraIndex_ = cameraIndex;
        skeletonEdges_         = std::move(skeletonEdges);
        frame_     = nullptr;
        exprFrame_ = nullptr;
        gazeFrame_ = nullptr;
        update();
    }

    // Clears both the drawn frame and the cached native video size — the
    // latter must be reset too (not just frame_), otherwise a subsequent
    // set_frame() for a newly-loaded video of a different resolution draws
    // keypoints scaled/offset against the PREVIOUS video's size until that
    // video's first QVideoSink frame arrives to refresh it.
    void clear() {
        frame_      = nullptr;
        exprFrame_  = nullptr;
        gazeFrame_  = nullptr;
        skeleton3dFrame_ = nullptr;
        nativeSize_ = QSize();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (nativeSize_.isEmpty() || width() <= 0 || height() <= 0) {
            return;
        }

        const double scale = std::min(
            static_cast<double>(width())  / nativeSize_.width(),
            static_cast<double>(height()) / nativeSize_.height());
        const double dispW = nativeSize_.width()  * scale;
        const double dispH = nativeSize_.height() * scale;
        const double offX  = (width()  - dispW) / 2.0;
        const double offY  = (height() - dispH) / 2.0;

        const auto to_widget = [&](QPointF p) {
            return QPointF(offX + p.x() * scale, offY + p.y() * scale);
        };

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        if (skeleton3dFrame_) {
            paint_skeleton3d(painter, to_widget);
            return;
        }
        if (gazeFrame_) {
            paint_gaze(painter, to_widget);
            return;
        }
        if (exprFrame_) {
            paint_expression(painter, to_widget);
            return;
        }
        if (!frame_) {
            return;
        }

        for (const auto& subject : frame_->subjects) {
            painter.setPen(QPen(QColor(255, 210, 0), 2));
            for (const auto& [a, b] : skeletonEdges_) {
                if (a < 0 || b < 0 || a >= subject.keypoints.size() ||
                    b >= subject.keypoints.size()) {
                    continue;
                }
                if (!is_keypoint_visible(subject, a) || !is_keypoint_visible(subject, b)) {
                    continue;
                }
                painter.drawLine(to_widget(subject.keypoints[a]),
                                  to_widget(subject.keypoints[b]));
            }

            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 220, 255));
            for (int i = 0; i < subject.keypoints.size(); ++i) {
                if (!is_keypoint_visible(subject, i)) { continue; }
                painter.drawEllipse(to_widget(subject.keypoints[i]), 3, 3);
            }
        }
    }

private:
    // Draws a bbox + "<expression> (<score>%)" label per detected face.
    // Split out from paintEvent() purely to keep that function readable —
    // not reused elsewhere.
    void paint_expression(QPainter& painter, const std::function<QPointF(QPointF)>& to_widget) {
        // painter.font() is constant across every subject in one paintEvent
        // call, so QFontMetrics only needs building once here, not per
        // subject inside the loop below.
        const QFontMetrics fm(painter.font());

        for (const auto& subject : exprFrame_->subjects) {
            const QRectF box(to_widget(subject.bbox.topLeft()),
                              to_widget(subject.bbox.bottomRight()));
            painter.setPen(QPen(QColor(255, 120, 60), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(box);

            const QString label = QString("%1 (%2%)")
                .arg(subject.dominantExpression)
                .arg(qRound(subject.dominantScore * 100));
            const QRectF labelBg(box.left(), box.top() - fm.height() - 4,
                                  fm.horizontalAdvance(label) + 8, fm.height() + 4);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 160));
            painter.drawRect(labelBg);
            painter.setPen(QColor(255, 210, 160));
            painter.drawText(labelBg, Qt::AlignCenter, label);
        }
    }

    // Draws a bbox + short direction arrow (from gazeDx/gazeDy, same
    // convention as the live QML monitor's gaze overlay in
    // src/qml/MonitorView.qml) for whichever GazeFusionCamera entry in
    // gazeFrame_->perCamera matches gazeCameraIndex_ — a fused frame may
    // carry contributions from several cameras, but this overlay only ever
    // draws the one matching the currently-loaded video.
    void paint_gaze(QPainter& painter, const std::function<QPointF(QPointF)>& to_widget) {
        const GazeFusionCamera* cam = nullptr;
        for (const auto& c : gazeFrame_->perCamera) {
            if (c.cameraIndex == gazeCameraIndex_) { cam = &c; break; }
        }
        if (!cam) { return; }

        const QFontMetrics fm(painter.font());

        const QRectF box(to_widget(cam->faceBoxPx.topLeft()),
                          to_widget(cam->faceBoxPx.bottomRight()));
        painter.setPen(QPen(QColor(80, 220, 220), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(box);

        // Arrow from box centre, extending toward (gazeDx, gazeDy) scaled by
        // the box's own size — same relative-to-face-size convention the
        // live monitor overlay uses, just computed in native-video pixel
        // space here (before to_widget scaling) rather than screen space.
        const QPointF centerNative = cam->faceBoxPx.center();
        const QPointF tipNative(
            centerNative.x() + cam->gazeDx * cam->faceBoxPx.width()  * 0.5,
            centerNative.y() + cam->gazeDy * cam->faceBoxPx.height() * 0.5);
        painter.setPen(QPen(QColor(255, 220, 60), 2));
        painter.drawLine(to_widget(centerNative), to_widget(tipNative));
        painter.setBrush(QColor(255, 220, 60));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(to_widget(tipNative), 3, 3);

        const QString label = QString("cam%1  dx%2 dy%3")
            .arg(cam->cameraIndex)
            .arg(cam->gazeDx, 0, 'f', 2)
            .arg(cam->gazeDy, 0, 'f', 2);
        const QRectF labelBg(box.left(), box.top() - fm.height() - 4,
                              fm.horizontalAdvance(label) + 8, fm.height() + 4);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 160));
        painter.drawRect(labelBg);
        painter.setPen(QColor(160, 230, 230));
        painter.drawText(labelBg, Qt::AlignCenter, label);
    }

    // Draws every reconstructed person's reprojected 2D skeleton for the
    // current camera — track_id decides color (kTrackColors), a keypoint
    // index is skipped entirely (not just left undrawn as a stray dot) if
    // its owning Skeleton3DKeypoint::valid is false, mirroring the JSON
    // schema's own "keypoints_valid is the single source of truth" rule.
    void paint_skeleton3d(QPainter& painter, const std::function<QPointF(QPointF)>& to_widget) {
        for (const auto& person : skeleton3dFrame_->people) {
            const auto pts = person.reprojectedPx.value(skeleton3dCameraIndex_);
            if (pts.isEmpty()) { continue; }

            const QColor color = kTrackColors[((person.trackId % 6) + 6) % 6];

            painter.setPen(QPen(color, 2));
            for (const auto& [a, b] : skeletonEdges_) {
                if (a < 0 || b < 0 || a >= pts.size() || b >= pts.size() ||
                    a >= person.keypoints.size() || b >= person.keypoints.size()) {
                    continue;
                }
                if (!person.keypoints[a].valid || !person.keypoints[b].valid) { continue; }
                painter.drawLine(to_widget(pts[a]), to_widget(pts[b]));
            }

            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            int firstValid = -1;
            for (int i = 0; i < pts.size(); ++i) {
                if (i >= person.keypoints.size() || !person.keypoints[i].valid) { continue; }
                painter.drawEllipse(to_widget(pts[i]), 3, 3);
                if (firstValid < 0) { firstValid = i; }
            }

            if (firstValid >= 0) {
                painter.setPen(color);
                painter.drawText(to_widget(pts[firstValid]) + QPointF(6, -6),
                                  QString("track %1").arg(person.trackId));
            }
        }
    }

    QSize                    nativeSize_;
    const PoseFrame*         frame_ = nullptr;
    QVector<QPair<int, int>> skeletonEdges_;
    const ExpressionFrame*   exprFrame_ = nullptr;
    const GazeFusionFrame*   gazeFrame_ = nullptr;
    int                      gazeCameraIndex_ = -1;
    const Skeleton3DFrame*   skeleton3dFrame_ = nullptr;
    int                      skeleton3dCameraIndex_ = -1;
};

// ── PoseOverlayPlayerW::Impl ────────────────────────────────────────────

struct PoseOverlayPlayerW::Impl {
    QMediaPlayer*      player  = nullptr;
    QVideoSink*        sink    = nullptr;
    QLabel*            display = nullptr;
    SkeletonOverlayW*  overlay = nullptr;

    QPushButton*       playBtn  = nullptr;
    QSlider*           scrubber = nullptr;
    QLabel*            timeLbl  = nullptr;

    PoseAnalysisResult poseResult;
    ExpressionResult   expressionResult;
    GazeFusionResult   gazeResult;
    int                gazeCameraIndex = -1;
    Skeleton3DResult   skeleton3dResult;
    int                skeleton3dCameraIndex = -1;
    QSize              nativeSize;
    double             fps        = 25.0;
    bool               scrubbing  = false;

    [[nodiscard]] int frame_estimate(int64_t positionMs) const {
        return static_cast<int>(std::llround(positionMs / 1000.0 * fps));
    }

    // GazeFusionResult::nearest_frame() is keyed on absolute timestampNs
    // (the shared master timeline — see its header doc), not a per-video
    // frame index like Pose/Expression's nearest_frame(). Approximates
    // "player position 0" as aligned with the gaze result's own first fused
    // tick — a small-skew approximation in the same spirit as this
    // codebase's other cross-camera-alignment conveniences, not a
    // replacement for SyncManifest's own precise alignment.
    [[nodiscard]] int64_t gaze_timestamp_estimate(int64_t positionMs) const {
        if (gazeResult.frames().isEmpty()) { return 0; }
        return gazeResult.frames().first().timestampNs + positionMs * 1000000LL;
    }

    // Same "player position 0 ≈ result's own first fused tick" approximation
    // as gaze_timestamp_estimate() above, for Skeleton3DResult's identically
    // timestamp-keyed nearest_frame().
    [[nodiscard]] int64_t skeleton3d_timestamp_estimate(int64_t positionMs) const {
        if (skeleton3dResult.frames().isEmpty()) { return 0; }
        return skeleton3dResult.frames().first().timestampNs + positionMs * 1000000LL;
    }
};

// ── Construction ─────────────────────────────────────────────────────────

PoseOverlayPlayerW::PoseOverlayPlayerW(QWidget* parent)
    : QWidget(parent), d(std::make_unique<Impl>())
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(4);

    auto* videoContainer = new QWidget;
    videoContainer->setMinimumSize(320, 240);
    auto* stack = new QStackedLayout(videoContainer);
    stack->setStackingMode(QStackedLayout::StackAll);
    stack->setContentsMargins(0, 0, 0, 0);

    d->display = new QLabel;
    d->display->setAlignment(Qt::AlignCenter);
    d->display->setStyleSheet("background:#080816;");
    stack->addWidget(d->display);

    d->overlay = new SkeletonOverlayW;
    stack->addWidget(d->overlay);

    // QStackedLayout::addWidget() only sets currentIndex on the FIRST widget
    // added (when there was none yet) — d->display, added above, silently
    // stayed "current" forever. In StackAll mode the current widget is the
    // one raised to the top of the z-order, so without this the overlay
    // was compositing BEHIND the opaque video label: painted correctly
    // (confirmed by direct code read), just never visible. This is the
    // actual root cause of "no markers ever show" — a real bug, not just
    // the SkeletonOverlayW::set_native_size() repaint gap fixed earlier
    // (that fix was still correct/needed, just not sufficient alone).
    stack->setCurrentWidget(d->overlay);

    outer->addWidget(videoContainer, 1);

    // ── Transport controls ──────────────────────────────────────────────
    auto* transport = new QHBoxLayout;
    d->playBtn = new QPushButton("▶");
    d->playBtn->setFixedWidth(32);
    transport->addWidget(d->playBtn);

    d->scrubber = new QSlider(Qt::Horizontal);
    transport->addWidget(d->scrubber, 1);

    d->timeLbl = new QLabel("0:00 / 0:00");
    d->timeLbl->setStyleSheet("color:#8888aa; font-size:11px;");
    transport->addWidget(d->timeLbl);
    outer->addLayout(transport);

    // ── Media player wiring ─────────────────────────────────────────────
    d->player = new QMediaPlayer(this);
    d->sink   = new QVideoSink(this);
    d->player->setVideoSink(d->sink);

    connect(d->sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& vf) {
        if (!vf.isValid()) { return; }
        if (d->nativeSize.isEmpty() && vf.size().isValid()) {
            d->nativeSize = vf.size();
            d->overlay->set_native_size(d->nativeSize);
        }
        QImage img = vf.toImage();
        if (img.isNull()) { return; }
        const QSize sz = d->display->size();
        if (sz.isEmpty()) { return; }
        d->display->setPixmap(QPixmap::fromImage(
            img.scaled(sz, Qt::KeepAspectRatio, Qt::FastTransformation)));
    });

    connect(d->player, &QMediaPlayer::metaDataChanged, this, [this] {
        const double fps = d->player->metaData()
                                .value(QMediaMetaData::VideoFrameRate)
                                .toDouble();
        if (fps > 0.0) { d->fps = fps; }
    });

    connect(d->player, &QMediaPlayer::durationChanged, this, [this](qint64 dur) {
        d->scrubber->setRange(0, static_cast<int>(dur));
        d->timeLbl->setText(format_ms(d->player->position()) + " / " + format_ms(dur));
    });

    connect(d->player, &QMediaPlayer::positionChanged, this, [this](qint64 pos) {
        if (!d->scrubbing) { d->scrubber->setValue(static_cast<int>(pos)); }
        d->timeLbl->setText(format_ms(pos) + " / " + format_ms(d->player->duration()));

        if (d->skeleton3dResult.is_valid()) {
            const Skeleton3DFrame* frame =
                d->skeleton3dResult.nearest_frame(d->skeleton3d_timestamp_estimate(pos));
            d->overlay->set_skeleton3d_frame(frame, d->skeleton3dCameraIndex,
                                              d->skeleton3dResult.skeleton_edges());
        } else if (d->gazeResult.is_valid()) {
            const GazeFusionFrame* frame =
                d->gazeResult.nearest_frame(d->gaze_timestamp_estimate(pos));
            d->overlay->set_gaze_frame(frame, d->gazeCameraIndex);
        } else if (d->expressionResult.is_valid()) {
            const ExpressionFrame* frame =
                d->expressionResult.nearest_frame(d->frame_estimate(pos));
            d->overlay->set_expression_frame(frame);
        } else {
            const PoseFrame* frame = d->poseResult.is_valid()
                ? d->poseResult.nearest_frame(d->frame_estimate(pos))
                : nullptr;
            d->overlay->set_frame(frame, d->poseResult.skeleton_edges());
        }

        emit position_changed(pos);
    });

    connect(d->player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState st) {
        d->playBtn->setText(st == QMediaPlayer::PlayingState ? "⏸" : "▶");
    });

    connect(d->playBtn, &QPushButton::clicked, this, [this] {
        if (d->player->playbackState() == QMediaPlayer::PlayingState) { pause(); }
        else { play(); }
    });

    connect(d->scrubber, &QSlider::sliderPressed,  this, [this] { d->scrubbing = true; });
    connect(d->scrubber, &QSlider::sliderReleased, this, [this] {
        d->scrubbing = false;
        seek(d->scrubber->value());
    });
}

PoseOverlayPlayerW::~PoseOverlayPlayerW() = default;

// ── Public API ───────────────────────────────────────────────────────────

void PoseOverlayPlayerW::set_video(const QString& videoPath) {
    d->nativeSize = QSize();
    d->overlay->clear();
    d->display->clear();   // drop the outgoing video's last frame, not just the overlay
    d->player->setSource(QUrl::fromLocalFile(videoPath));
}

void PoseOverlayPlayerW::set_pose_result(const PoseAnalysisResult& result) {
    d->expressionResult = ExpressionResult();   // mutually exclusive, see header doc
    d->gazeResult        = GazeFusionResult();
    d->skeleton3dResult  = Skeleton3DResult();
    d->poseResult = result;
    const PoseFrame* frame = d->poseResult.is_valid()
        ? d->poseResult.nearest_frame(d->frame_estimate(d->player->position()))
        : nullptr;
    d->overlay->set_frame(frame, d->poseResult.skeleton_edges());
}

void PoseOverlayPlayerW::set_expression_result(const ExpressionResult& result) {
    d->poseResult = PoseAnalysisResult();   // mutually exclusive, see header doc
    d->gazeResult = GazeFusionResult();
    d->skeleton3dResult = Skeleton3DResult();
    d->expressionResult = result;
    const ExpressionFrame* frame = d->expressionResult.is_valid()
        ? d->expressionResult.nearest_frame(d->frame_estimate(d->player->position()))
        : nullptr;
    d->overlay->set_expression_frame(frame);
}

void PoseOverlayPlayerW::set_gaze_result(const GazeFusionResult& result, int cameraIndex) {
    d->poseResult       = PoseAnalysisResult();   // mutually exclusive, see header doc
    d->expressionResult = ExpressionResult();
    d->skeleton3dResult  = Skeleton3DResult();
    d->gazeResult        = result;
    d->gazeCameraIndex   = cameraIndex;
    const GazeFusionFrame* frame = d->gazeResult.is_valid()
        ? d->gazeResult.nearest_frame(d->gaze_timestamp_estimate(d->player->position()))
        : nullptr;
    d->overlay->set_gaze_frame(frame, cameraIndex);
}

void PoseOverlayPlayerW::set_skeleton3d_result(const Skeleton3DResult& result, int cameraIndex) {
    d->poseResult          = PoseAnalysisResult();   // mutually exclusive, see header doc
    d->expressionResult    = ExpressionResult();
    d->gazeResult           = GazeFusionResult();
    d->skeleton3dResult     = result;
    d->skeleton3dCameraIndex = cameraIndex;
    const Skeleton3DFrame* frame = d->skeleton3dResult.is_valid()
        ? d->skeleton3dResult.nearest_frame(d->skeleton3d_timestamp_estimate(d->player->position()))
        : nullptr;
    d->overlay->set_skeleton3d_frame(frame, cameraIndex, d->skeleton3dResult.skeleton_edges());
}

int64_t PoseOverlayPlayerW::position_ms() const { return d->player->position(); }
int64_t PoseOverlayPlayerW::duration_ms() const { return d->player->duration(); }

void PoseOverlayPlayerW::play()  { d->player->play(); }
void PoseOverlayPlayerW::pause() { d->player->pause(); }

void PoseOverlayPlayerW::seek(int64_t positionMs) {
    d->player->setPosition(positionMs);
}

} // namespace mosaic
