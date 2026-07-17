#include "ui/analysis/pose_overlay_player_w.hpp"
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

namespace mosaic {

namespace {

QString format_ms(int64_t ms) {
    const int64_t totalSec = ms / 1000;
    return QString("%1:%2")
        .arg(totalSec / 60, 2, 10, QChar('0'))
        .arg(totalSec % 60, 2, 10, QChar('0'));
}

} // namespace

// ── SkeletonOverlayW — transparent widget drawing the current pose frame ──

class SkeletonOverlayW : public QWidget {
public:
    explicit SkeletonOverlayW(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setStyleSheet("background:transparent;");
    }

    void set_native_size(QSize size) { nativeSize_ = size; }

    void set_frame(const PoseFrame* frame, QVector<QPair<int, int>> skeletonEdges) {
        frame_          = frame;
        skeletonEdges_  = std::move(skeletonEdges);
        update();
    }

    // Clears both the drawn frame and the cached native video size — the
    // latter must be reset too (not just frame_), otherwise a subsequent
    // set_frame() for a newly-loaded video of a different resolution draws
    // keypoints scaled/offset against the PREVIOUS video's size until that
    // video's first QVideoSink frame arrives to refresh it.
    void clear() {
        frame_     = nullptr;
        nativeSize_ = QSize();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (!frame_ || nativeSize_.isEmpty() || width() <= 0 || height() <= 0) {
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
    QSize                    nativeSize_;
    const PoseFrame*         frame_ = nullptr;
    QVector<QPair<int, int>> skeletonEdges_;
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
    QSize              nativeSize;
    double             fps        = 25.0;
    bool               scrubbing  = false;

    [[nodiscard]] int frame_estimate(int64_t positionMs) const {
        return static_cast<int>(std::llround(positionMs / 1000.0 * fps));
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

        const PoseFrame* frame = d->poseResult.is_valid()
            ? d->poseResult.nearest_frame(d->frame_estimate(pos))
            : nullptr;
        d->overlay->set_frame(frame, d->poseResult.skeleton_edges());

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
    d->poseResult = result;
    const PoseFrame* frame = d->poseResult.is_valid()
        ? d->poseResult.nearest_frame(d->frame_estimate(d->player->position()))
        : nullptr;
    d->overlay->set_frame(frame, d->poseResult.skeleton_edges());
}

int64_t PoseOverlayPlayerW::position_ms() const { return d->player->position(); }
int64_t PoseOverlayPlayerW::duration_ms() const { return d->player->duration(); }

void PoseOverlayPlayerW::play()  { d->player->play(); }
void PoseOverlayPlayerW::pause() { d->player->pause(); }

void PoseOverlayPlayerW::seek(int64_t positionMs) {
    d->player->setPosition(positionMs);
}

} // namespace mosaic
