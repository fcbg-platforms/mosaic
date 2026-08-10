#include "ui/realtime/realtime_camera_tile_w.hpp"
#include "analysis/realtime_metrics.hpp"
#include "ui/anim_utils.hpp"
#include "ui/calibration/badge_style.hpp"
#include "ui/realtime/pose_skeleton_edges.hpp"
#include <QCheckBox>
#include <QDateTime>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <algorithm>
#include <cmath>
#include <functional>

namespace mosaic {

namespace {

QColor quality_color(RmsQuality q) {
    switch (q) {
        case RmsQuality::Excellent:
        case RmsQuality::Good:       return QColor("#44cc88");
        case RmsQuality::Acceptable: return QColor("#ddaa44");
        case RmsQuality::Poor:       return QColor("#cc4444");
    }
    return QColor("#cc4444");
}

// Same pill shape as badge_style.cpp's private pill_stylesheet(), just
// parameterized by an arbitrary (possibly lerped, mid-transition) color
// instead of one of the three fixed tier colors — used only while the badge
// is animating between tiers; the animation always ends by committing the
// canonical badge_stylesheet(quality) string so the final state matches
// every other quality badge in the app exactly.
QString pill_stylesheet_for_color(const QColor& c) {
    return QString(
        "QLabel { color: %1; background: rgba(%2,%3,%4,0.15); border: 1px solid %1; "
        "border-radius: 4px; padding: 1px 8px; font-weight: 600; }")
        .arg(c.name())
        .arg(c.red()).arg(c.green()).arg(c.blue());
}

} // namespace

// ── ThumbnailAreaW — video frame + pose/gaze overlay + compass ────────────

class ThumbnailAreaW : public QWidget {
public:
    explicit ThumbnailAreaW(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(160, 90);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void set_frame(const QImage& frame) {
        const bool wasEmpty = frame_.isNull();
        frame_ = frame;
        update();
        if (wasEmpty && !frame_.isNull()) { emit_first_frame(); }
    }

    void set_pose(const QVariantList& keypoints) {
        keypoints_ = keypoints;
        update();
    }

    void set_gaze(std::optional<QVariantMap> gaze) {
        gaze_ = std::move(gaze);
        update();
    }

    std::function<void()> on_first_frame;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor("#0e0e22"));

        if (frame_.isNull() || width() <= 0 || height() <= 0) {
            p.setPen(QColor("#33334a"));
            p.drawText(rect(), Qt::AlignCenter, "No signal");
            return;
        }

        const double scale = std::min(static_cast<double>(width())  / frame_.width(),
                                       static_cast<double>(height()) / frame_.height());
        const double dispW = frame_.width()  * scale;
        const double dispH = frame_.height() * scale;
        const double offX  = (width()  - dispW) / 2.0;
        const double offY  = (height() - dispH) / 2.0;

        p.drawImage(QRectF(offX, offY, dispW, dispH), frame_);

        // Pose/gaze coordinates from the live pipeline are normalized [0,1]
        // fractions of the source frame (MediaPipe Tasks convention — see
        // python/pose/estimator.py / gaze_estimator.py), so mapping into
        // widget space is a direct scale, no separate native-size lookup
        // needed (unlike the post-hoc pixel-coordinate overlay this mirrors).
        const auto to_widget = [&](double nx, double ny) {
            return QPointF(offX + nx * dispW, offY + ny * dispH);
        };

        paint_pose(p, to_widget);
        paint_gaze(p, to_widget);
        paint_compass(p);
    }

private:
    void paint_pose(QPainter& p, const std::function<QPointF(double, double)>& to_widget) {
        if (keypoints_.isEmpty()) { return; }

        QHash<QString, int> nameToIndex;
        nameToIndex.reserve(keypoints_.size());
        for (int i = 0; i < keypoints_.size(); ++i) {
            nameToIndex.insert(keypoints_[i].toMap().value("name").toString(), i);
        }
        const auto visible_at = [&](int idx) -> std::optional<QPointF> {
            if (idx < 0 || idx >= keypoints_.size()) { return std::nullopt; }
            const auto kp = keypoints_[idx].toMap();
            if (kp.value("visibility").toDouble() < 0.4) { return std::nullopt; }
            return to_widget(kp.value("x").toDouble(), kp.value("y").toDouble());
        };

        // Colors match SkeletonOverlayW's post-hoc pose overlay exactly
        // (pose_overlay_player_w.cpp) for visual consistency between the
        // live and post-hoc views.
        p.setPen(QPen(QColor(255, 210, 0), 2));
        for (const auto& edge : kPoseNameEdges) {
            const int ai = nameToIndex.value(QString::fromLatin1(edge.a), -1);
            const int bi = nameToIndex.value(QString::fromLatin1(edge.b), -1);
            const auto pa = visible_at(ai);
            const auto pb = visible_at(bi);
            if (pa && pb) { p.drawLine(*pa, *pb); }
        }

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 220, 255));
        for (int i = 0; i < keypoints_.size(); ++i) {
            if (auto pt = visible_at(i)) { p.drawEllipse(*pt, 3.0, 3.0); }
        }
    }

    void paint_gaze(QPainter& p, const std::function<QPointF(double, double)>& to_widget) {
        if (!gaze_) { return; }
        const auto fb = gaze_->value("face_box").toMap();
        const double fx = fb.value("x").toDouble(), fy = fb.value("y").toDouble();
        const double fw = fb.value("w").toDouble(), fh = fb.value("h").toDouble();

        // Same colors as SkeletonOverlayW::paint_gaze()'s post-hoc overlay.
        p.setPen(QPen(QColor(80, 220, 220), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(to_widget(fx, fy), to_widget(fx + fw, fy + fh)));

        const double gdx = gaze_->value("gaze_dx").toDouble();
        const double gdy = gaze_->value("gaze_dy").toDouble();
        const QPointF center = to_widget(fx + fw / 2.0, fy + fh / 2.0);
        const QPointF tip     = to_widget(fx + fw / 2.0 + gdx * fw * 0.5,
                                           fy + fh / 2.0 + gdy * fh * 0.5);
        p.setPen(QPen(QColor(255, 220, 60), 2));
        p.drawLine(center, tip);
        p.setBrush(QColor(255, 220, 60));
        p.setPen(Qt::NoPen);
        p.drawEllipse(tip, 3.0, 3.0);
    }

    // Small (~28px) top-right ring showing the current gaze offset — purely
    // a per-camera 2D indicator (see the roadmap plan's explicit "no fake
    // 3D/floorplan fusion" decision — this never claims a room position).
    void paint_compass(QPainter& p) {
        constexpr double kSize   = 28.0;
        constexpr double kMargin = 6.0;
        const QRectF ring(width() - kSize - kMargin, kMargin, kSize, kSize);

        p.setPen(QPen(QColor(80, 80, 130), 1));
        p.setBrush(QColor(10, 10, 24, 180));
        p.drawEllipse(ring);

        if (gaze_) {
            const double gdx = std::clamp(gaze_->value("gaze_dx").toDouble(), -1.0, 1.0);
            const double gdy = std::clamp(gaze_->value("gaze_dy").toDouble(), -1.0, 1.0);
            const double r = kSize / 2.0 - 4.0;
            const QPointF dot(ring.center().x() + gdx * r, ring.center().y() + gdy * r);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 220, 60));
            p.drawEllipse(dot, 3.0, 3.0);
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(80, 80, 130));
            p.drawEllipse(ring.center(), 2.0, 2.0);
        }
    }

    void emit_first_frame() { if (on_first_frame) { on_first_frame(); } }

    QImage                      frame_;
    QVariantList                keypoints_;
    std::optional<QVariantMap>  gaze_;
};

// ── Impl ────────────────────────────────────────────────────────────────────

struct RealtimeCameraTileW::Impl {
    int  cameraIndex    = 0;
    bool analyzeEnabled = true;
    bool paused         = false;

    QLabel*         liveDot         = nullptr;
    QCheckBox*      analyzeCk       = nullptr;
    ThumbnailAreaW* thumb           = nullptr;
    QWidget*        footer          = nullptr;
    QLabel*         qualityBadge    = nullptr;
    QLabel*         gazeOnTargetLbl = nullptr;

    DetectionRateTracker detectionTracker;
    DetectionRateTracker gazeTracker;
    bool gazeSeenThisTick = false;

    bool       haveQuality = false;
    RmsQuality lastQuality = RmsQuality::Poor;
    QPointer<QVariantAnimation> badgeAnim;
};

// ── RealtimeCameraTileW ─────────────────────────────────────────────────────

RealtimeCameraTileW::RealtimeCameraTileW(int cameraIndex, bool liveAnalysisEnabled,
                                          int detectionWindowBuckets, QWidget* parent)
    : QWidget(parent), d(std::make_unique<Impl>())
{
    d->cameraIndex       = cameraIndex;
    d->analyzeEnabled    = liveAnalysisEnabled;
    d->detectionTracker  = DetectionRateTracker(std::max(1, detectionWindowBuckets));
    d->gazeTracker       = DetectionRateTracker(std::max(1, detectionWindowBuckets));

    setStyleSheet("RealtimeCameraTileW { background:#13132a; border:1px solid #252545; "
                  "border-radius: 6px; }");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    auto* header = new QHBoxLayout;
    d->liveDot = new QLabel;
    d->liveDot->setFixedSize(10, 10);
    d->liveDot->setStyleSheet("background:#44446a; border-radius:5px;");
    auto* camLbl = new QLabel(QString("Cam %1").arg(cameraIndex));
    camLbl->setStyleSheet("color:#c8c8e0; font-weight:600;");
    d->analyzeCk = new QCheckBox("Analyze");
    d->analyzeCk->setChecked(liveAnalysisEnabled);
    d->analyzeCk->setToolTip(
        "Include this camera in the shared live pose/gaze budget "
        "(~5fps/camera, split across every enabled camera).");
    connect(d->analyzeCk, &QCheckBox::toggled, this, [this](bool on) {
        d->analyzeEnabled = on;
        if (!on) {
            // Turning analysis off stops new pose_ready/gaze_ready events
            // from ever arriving for this camera (main_window.cpp's
            // frame_preview lambda skips submit_frame() once
            // liveAnalysisEnabled is false) — without this, the thumbnail
            // keeps drawing whatever skeleton/gaze it last received,
            // frozen indefinitely instead of disappearing.
            d->thumb->set_pose({});
            d->thumb->set_gaze(std::nullopt);
        }
        emit analyze_toggled(d->cameraIndex, on);
    });
    header->addWidget(d->liveDot);
    header->addWidget(camLbl);
    header->addStretch(1);
    header->addWidget(d->analyzeCk);
    root->addLayout(header);

    d->thumb = new ThumbnailAreaW;
    d->thumb->on_first_frame = [this] {
        d->liveDot->setStyleSheet("background:#44ff88; border-radius:5px;");
        setStyleSheet("RealtimeCameraTileW { background:#13132a; border:1px solid #2a3060; "
                      "border-radius: 6px; }");
        anim::fade_in_widget(d->thumb, 200);
    };
    root->addWidget(d->thumb, 1);

    d->footer = new QWidget;
    auto* footerLay = new QHBoxLayout(d->footer);
    footerLay->setContentsMargins(0, 0, 0, 0);
    d->qualityBadge = new QLabel("Pose: --");
    d->qualityBadge->setStyleSheet(badge_stylesheet(RmsQuality::Poor));
    d->gazeOnTargetLbl = new QLabel("On-target: --");
    d->gazeOnTargetLbl->setStyleSheet("color:#c8c8e0;");
    footerLay->addWidget(d->qualityBadge);
    footerLay->addStretch(1);
    footerLay->addWidget(d->gazeOnTargetLbl);
    root->addWidget(d->footer);
}

RealtimeCameraTileW::~RealtimeCameraTileW() = default;

int  RealtimeCameraTileW::camera_index() const { return d->cameraIndex; }
bool RealtimeCameraTileW::analyze_enabled() const { return d->analyzeEnabled; }

std::optional<double> RealtimeCameraTileW::detection_rate() const {
    return d->detectionTracker.rate();
}

std::optional<double> RealtimeCameraTileW::gaze_on_target_rate() const {
    return d->gazeTracker.rate();
}

void RealtimeCameraTileW::set_analysis_paused(bool paused) {
    if (d->paused == paused) { return; }
    d->paused = paused;
    anim::ensure_opacity_effect(d->footer)->setOpacity(paused ? 0.45 : 1.0);
}

void RealtimeCameraTileW::on_frame_preview(QImage frame) {
    d->thumb->set_frame(frame);
}

void RealtimeCameraTileW::on_pose_ready(QVariantList keypoints) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Attribute the PREVIOUS frame's gaze presence: gaze_ready (if any) for
    // that frame fired between the previous pose_ready call and this one.
    d->gazeTracker.push(d->gazeSeenThisTick, now);
    if (!d->gazeSeenThisTick) { d->thumb->set_gaze(std::nullopt); }
    d->gazeSeenThisTick = false;

    d->detectionTracker.push(!keypoints.isEmpty(), now);
    d->thumb->set_pose(keypoints);

    const auto rate = d->detectionTracker.rate();
    const RmsQuality quality = pose_tracking_quality_for(rate.value_or(0.0));
    d->qualityBadge->setText(rate ? QString("Pose: %1%").arg(qRound(*rate * 100))
                                   : QStringLiteral("Pose: --"));

    if (!d->haveQuality) {
        d->haveQuality  = true;
        d->lastQuality  = quality;
        d->qualityBadge->setStyleSheet(badge_stylesheet(quality));
    } else if (quality != d->lastQuality) {
        const QColor from = quality_color(d->lastQuality);
        const QColor to   = quality_color(quality);
        d->lastQuality = quality;

        if (d->badgeAnim) { d->badgeAnim->stop(); }
        auto* anim = new QVariantAnimation(this);
        anim->setStartValue(0.0);
        anim->setEndValue(1.0);
        anim->setDuration(300);
        QPointer<QLabel> badge = d->qualityBadge;
        connect(anim, &QVariantAnimation::valueChanged, badge, [badge, from, to](const QVariant& v) {
            if (badge) { badge->setStyleSheet(pill_stylesheet_for_color(anim::lerp_color(from, to, v.toReal()))); }
        });
        connect(anim, &QVariantAnimation::finished, badge, [badge, quality]() {
            if (badge) { badge->setStyleSheet(badge_stylesheet(quality)); }
        });
        anim->start(QAbstractAnimation::DeleteWhenStopped);
        d->badgeAnim = anim;
    }

    const auto gRate = d->gazeTracker.rate();
    d->gazeOnTargetLbl->setText(gRate ? QString("On-target: %1%").arg(qRound(*gRate * 100))
                                       : QStringLiteral("On-target: --"));
}

void RealtimeCameraTileW::on_gaze_ready(QVariantMap gazeData) {
    d->gazeSeenThisTick = true;
    d->thumb->set_gaze(gazeData);
}

} // namespace mosaic
