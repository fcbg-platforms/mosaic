#include "ui/session/session_player_w.hpp"
#include "utils/logger.hpp"
#include <QAudioOutput>
#include <QBoxLayout>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGroupBox>
#include <QKeyEvent>
#include <QLabel>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QSlider>
#include <QSplitter>
#include <QStackedLayout>
#include <QTimer>
#include <QToolButton>
#include <QVideoFrame>
#include <QVideoSink>
#include <algorithm>
#include <cmath>

namespace mosaic {

// ── AnnotationBarW ────────────────────────────────────────────────────────
// Thin custom widget that paints annotation blocks on a timeline strip and
// draws a cursor for the current playback position.

class AnnotationBarW : public QWidget {
    Q_OBJECT
public:
    explicit AnnotationBarW(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setFixedHeight(22);
        setToolTip("Annotation timeline — click to seek");
    }

    void set_data(const QList<Annotation>& anns, int64_t durationMs) {
        annotations_ = anns;
        durationMs_  = durationMs;
        update();
    }

    void set_position(int64_t posMs) {
        posMs_ = posMs;
        update();
    }

signals:
    void seek_requested(int64_t posMs);

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // Background
        p.fillRect(rect(), QColor("#0d0d22"));
        p.setPen(QColor("#1e1e3a"));
        p.drawRect(rect().adjusted(0, 0, -1, -1));

        if (durationMs_ <= 0) { return; }

        // Annotation blocks
        for (const auto& ann : annotations_) {
            const double frac = static_cast<double>(ann.timestampMs) / durationMs_;
            const int    x    = static_cast<int>(frac * width());
            const QColor col  = AnnotationCategory::color_for(ann.category);
            p.fillRect(x, 2, std::max(3, static_cast<int>(width() * 0.015)),
                       height() - 4, col.darker(120));
            p.setPen(col);
            p.drawRect(x, 2, std::max(3, static_cast<int>(width() * 0.015)),
                       height() - 5);
        }

        // Cursor
        if (posMs_ >= 0 && durationMs_ > 0) {
            const int cx = static_cast<int>(
                static_cast<double>(posMs_) / durationMs_ * width());
            p.setPen(QPen(QColor("#aaaaee"), 1.5));
            p.drawLine(cx, 0, cx, height());
        }
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (durationMs_ <= 0) { return; }
        const int64_t pos = static_cast<int64_t>(
            static_cast<double>(e->pos().x()) / width() * durationMs_);
        emit seek_requested(std::clamp(pos, 0LL, durationMs_));
    }

private:
    QList<Annotation> annotations_;
    int64_t durationMs_ = 0;
    int64_t posMs_      = -1;
};

// ── CameraSlotW ──────────────────────────────────────────────────────────
// One cell in the camera grid: video display + overlay badges.

class CameraSlotW : public QWidget {
public:
    explicit CameraSlotW(int camIdx, QWidget* parent = nullptr)
        : QWidget(parent), camIdx_(camIdx)
    {
        setMinimumSize(160, 100);

        auto* lay = new QStackedLayout(this);
        lay->setStackingMode(QStackedLayout::StackAll);
        lay->setContentsMargins(0, 0, 0, 0);

        // Black base
        base_ = new QLabel(this);
        base_->setAlignment(Qt::AlignCenter);
        base_->setStyleSheet("background:#080816;");
        base_->setText(QString("Cam %1").arg(camIdx_ + 1));
        base_->setStyleSheet(
            "QLabel{background:#080816;color:#1a1a38;font-size:11px;}");
        lay->addWidget(base_);

        // Video display (pixmap updated per frame)
        display_ = new QLabel(this);
        display_->setAlignment(Qt::AlignCenter);
        display_->setAttribute(Qt::WA_TransparentForMouseEvents);
        display_->setStyleSheet("background:transparent;");
        lay->addWidget(display_);

        // Overlay widget for badges
        overlay_ = new QWidget(this);
        overlay_->setAttribute(Qt::WA_TransparentForMouseEvents);
        overlay_->setStyleSheet("background:transparent;");
        lay->addWidget(overlay_);

        // Camera index badge (top-left)
        camBadge_ = new QLabel(overlay_);
        camBadge_->setText(QString("Cam %1").arg(camIdx_ + 1));
        camBadge_->setStyleSheet(
            "QLabel{background:#12122a;border:1px solid #252545;"
            "border-radius:8px;padding:1px 7px;color:#6666aa;font-size:9px;"
            "font-weight:bold;}");
        camBadge_->adjustSize();
        camBadge_->move(6, 6);

        // Stats badge (bottom-left)
        statsBadge_ = new QLabel(overlay_);
        statsBadge_->setStyleSheet(
            "QLabel{background:#08081a;border:1px solid #1e1e36;"
            "border-radius:6px;padding:1px 6px;font-size:8px;}");
        statsBadge_->setText("—");
        statsBadge_->adjustSize();

        // Live dot (top-right)
        liveDot_ = new QLabel(overlay_);
        liveDot_->setFixedSize(8, 8);
        liveDot_->setStyleSheet(
            "QLabel{background:#554422;border-radius:4px;}");
    }

    void update_frame(const QVideoFrame& vf) {
        if (!vf.isValid()) { return; }
        QImage img = vf.toImage();
        if (img.isNull()) { return; }
        const QSize sz = display_->size();
        if (sz.isEmpty()) { return; }
        display_->setPixmap(QPixmap::fromImage(
            img.scaled(sz, Qt::KeepAspectRatio, Qt::FastTransformation)));
        hasFrame_ = true;
        update_dot(true);
    }

    void set_stats(double fpsActual, double coveragePct) {
        const QColor col = coveragePct >= 75 ? QColor("#44cc66")
                         : coveragePct >= 45 ? QColor("#ddaa33")
                                             : QColor("#cc4444");
        statsBadge_->setStyleSheet(
            QString("QLabel{background:#08081a;border:1px solid #1e1e36;"
                    "border-radius:6px;padding:1px 6px;font-size:8px;color:%1;}")
                .arg(col.name()));
        statsBadge_->setText(
            QString("%.1f fps  %.0f%%").arg(fpsActual).arg(coveragePct));
        statsBadge_->adjustSize();
        reposition_badges();
    }

    void set_no_video() {
        display_->clear();
        base_->setText(QString("Cam %1\nNo video").arg(camIdx_ + 1));
        update_dot(false);
    }

    void update_dot(bool live) {
        liveDot_->setStyleSheet(live
            ? "QLabel{background:#33cc66;border-radius:4px;}"
            : "QLabel{background:#554422;border-radius:4px;}");
    }

protected:
    void resizeEvent(QResizeEvent*) override { reposition_badges(); }

private:
    void reposition_badges() {
        camBadge_->move(6, 6);
        statsBadge_->move(6, height() - statsBadge_->height() - 6);
        liveDot_->move(width() - liveDot_->width() - 7, 7);
    }

    int       camIdx_;
    bool      hasFrame_  = false;
    QLabel*   base_;
    QLabel*   display_;
    QWidget*  overlay_;
    QLabel*   camBadge_;
    QLabel*   statsBadge_;
    QLabel*   liveDot_;
};

// ── PlayerUnit ────────────────────────────────────────────────────────────

struct PlayerUnit {
    QMediaPlayer*  player       = nullptr;
    QVideoSink*    sink         = nullptr;
    CameraSlotW*   slot         = nullptr;
    int64_t        seekOffsetMs = 0;
    bool           hasVideo     = false;
};

// ── SessionPlayerW::Impl ─────────────────────────────────────────────────

struct SessionPlayerW::Impl {
    SessionInfo   session;
    SyncManifest  manifest;

    QVector<PlayerUnit> units;
    QMediaPlayer*       audioPlayer  = nullptr;
    QAudioOutput*       audioOutput  = nullptr;

    // Transport state
    bool      playing      = false;
    int64_t   masterMs     = 0;    // current playback position
    int64_t   durationMs   = 0;
    double    rate         = 1.0;
    int64_t   masterBase   = 0;    // masterMs at last timer reset

    QElapsedTimer  masterClock;
    QTimer         masterTimer;    // ~20 ms for smooth scrubber updates
    QTimer         syncTimer;      // 500 ms for drift correction

    // UI widgets
    QGridLayout*   cameraGrid    = nullptr;
    QSlider*       scrubber      = nullptr;
    QLabel*        timeLabel     = nullptr;
    QLabel*        durationLabel = nullptr;
    QPushButton*   playBtn       = nullptr;
    QToolButton*   stepBackBtn   = nullptr;
    QToolButton*   stepFwdBtn    = nullptr;
    QPushButton*   stopBtn       = nullptr;
    QComboBox*     speedBox      = nullptr;
    QSlider*       volSlider     = nullptr;
    AnnotationBarW* annotBar     = nullptr;
    QLabel*        qualityLabel  = nullptr;

    bool scrubbing = false;   // true while user drags scrubber

    int64_t current_master_ms() const {
        if (!playing) { return masterMs; }
        return masterBase + static_cast<int64_t>(masterClock.elapsed() * rate);
    }
};

// ── Constructor ───────────────────────────────────────────────────────────

SessionPlayerW::SessionPlayerW(const SessionInfo& session, QWidget* parent)
    : QDialog(parent, Qt::Window)
    , d(std::make_unique<Impl>())
{
    d->session = session;
    setWindowTitle(QString("Player — %1").arg(session.name));
    resize(1200, 750);

    // Generate or load manifest.
    const QString manifestPath = session.path + "/sync_manifest.json";
    if (QFileInfo::exists(manifestPath)) {
        d->manifest = SyncManifest::load(session.path);
        log_info("[Player] Loaded existing sync manifest.");
    } else {
        log_info("[Player] Generating sync manifest…");
        d->manifest = SyncManifest::generate(session.path);
        if (d->manifest.is_valid()) {
            d->manifest.save(session.path);
        }
    }

    d->durationMs = d->manifest.is_valid()
        ? d->manifest.duration_ms()
        : std::max(1LL, static_cast<int64_t>(session.durationMs));

    build_ui();
    setup_players();

    // Master display timer (~20 ms → ~50 fps scrubber / time updates).
    connect(&d->masterTimer, &QTimer::timeout, this, &SessionPlayerW::on_master_timer_tick);
    d->masterTimer.start(20);

    // Drift-correction timer (every 500 ms while playing).
    connect(&d->syncTimer, &QTimer::timeout, this, &SessionPlayerW::on_sync_tick);
    d->syncTimer.start(500);
}

SessionPlayerW::~SessionPlayerW()
{
    for (auto& u : d->units) {
        if (u.player) { u.player->stop(); }
    }
    if (d->audioPlayer) { d->audioPlayer->stop(); }
}

// ── UI construction ───────────────────────────────────────────────────────

void SessionPlayerW::build_ui()
{
    setStyleSheet(
        "QDialog{background:#080816;}"
        "QLabel{color:#aaaacc;}"
        "QPushButton{background:#12122a;border:1px solid #2a2a4a;"
        "  border-radius:5px;color:#aaaacc;padding:4px 12px;}"
        "QPushButton:hover{background:#1a1a38;border-color:#44447a;}"
        "QPushButton:pressed{background:#0d0d22;}"
        "QToolButton{background:#12122a;border:1px solid #2a2a4a;"
        "  border-radius:4px;color:#aaaacc;padding:2px 6px;}"
        "QToolButton:hover{background:#1a1a38;}"
        "QComboBox{background:#12122a;border:1px solid #2a2a4a;"
        "  border-radius:4px;color:#aaaacc;padding:2px 8px;}"
        "QComboBox QAbstractItemView{background:#12122a;color:#aaaacc;"
        "  selection-background-color:#1e1e50;}"
        "QSlider::groove:horizontal{background:#1a1a38;height:4px;border-radius:2px;}"
        "QSlider::handle:horizontal{background:#5555aa;width:12px;height:12px;"
        "  margin:-4px 0;border-radius:6px;}"
        "QSlider::sub-page:horizontal{background:#44447a;border-radius:2px;}"
    );

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(6);

    // ── Header ─────────────────────────────────────────────────────────────
    auto* header = new QHBoxLayout;
    auto* titleLbl = new QLabel(
        QString("▶  %1   <span style='color:#44446a;font-size:10px;'>%2</span>")
            .arg(d->session.name)
            .arg(d->session.recordedBy.isEmpty() ? "" : "@" + d->session.recordedBy));
    titleLbl->setStyleSheet("color:#8888bb;font-size:13px;font-weight:bold;");
    header->addWidget(titleLbl);
    header->addStretch();

    // Quality summary
    d->qualityLabel = new QLabel;
    d->qualityLabel->setStyleSheet("color:#44446a;font-size:9px;");
    if (d->manifest.is_valid()) {
        int goodCams = 0;
        for (int c = 0; c < d->manifest.camera_count(); ++c) {
            if (d->manifest.camera_info(c).coveragePct >= 50.0) { ++goodCams; }
        }
        d->qualityLabel->setText(
            QString("Sync: %1 / %2 cameras  ≥50% coverage")
                .arg(goodCams).arg(d->manifest.camera_count()));
    }
    header->addWidget(d->qualityLabel);
    root->addLayout(header);

    // ── Camera grid ────────────────────────────────────────────────────────
    const int nCams    = d->manifest.is_valid() ? d->manifest.camera_count()
                                                 : d->session.cameraCount;
    const int gridCols = (nCams <= 2) ? nCams
                       : (nCams <= 4) ? 2
                       : (nCams <= 6) ? 3 : 4;

    auto* gridWidget = new QWidget;
    gridWidget->setStyleSheet("background:#050510;border-radius:6px;");
    d->cameraGrid = new QGridLayout(gridWidget);
    d->cameraGrid->setContentsMargins(4, 4, 4, 4);
    d->cameraGrid->setSpacing(4);

    for (int i = 0; i < std::max(nCams, 1); ++i) {
        PlayerUnit u;
        u.slot = new CameraSlotW(i, gridWidget);
        u.seekOffsetMs = d->manifest.is_valid()
            ? d->manifest.seek_offset_ms(i) : 0;
        if (d->manifest.is_valid() && i < d->manifest.camera_count()) {
            const auto& cs = d->manifest.camera_info(i);
            u.slot->set_stats(cs.fpsActual, cs.coveragePct);
        }
        d->units.append(u);

        const int row = i / gridCols;
        const int col = i % gridCols;
        d->cameraGrid->addWidget(u.slot, row, col);
        d->cameraGrid->setRowStretch(row, 1);
        d->cameraGrid->setColumnStretch(col, 1);
    }
    root->addWidget(gridWidget, 1);

    // ── Annotation bar ─────────────────────────────────────────────────────
    d->annotBar = new AnnotationBarW;
    d->annotBar->set_data(d->session.annotations, d->durationMs);
    connect(d->annotBar, &AnnotationBarW::seek_requested,
            this, [this](int64_t posMs) { seek_all(posMs); });
    root->addWidget(d->annotBar);

    // ── Scrubber ───────────────────────────────────────────────────────────
    auto* scrubRow = new QHBoxLayout;
    scrubRow->setSpacing(8);

    d->timeLabel = new QLabel("00:00:00.000");
    d->timeLabel->setStyleSheet(
        "color:#ccccee;font-size:14px;font-family:'Courier New';min-width:90px;");

    d->scrubber = new QSlider(Qt::Horizontal);
    d->scrubber->setRange(0, static_cast<int>(d->durationMs));
    d->scrubber->setSingleStep(40);
    d->scrubber->setPageStep(1000);
    connect(d->scrubber, &QSlider::sliderPressed,  this, [this]{ d->scrubbing = true; });
    connect(d->scrubber, &QSlider::sliderReleased, this, [this]{
        d->scrubbing = false;
        seek_all(d->scrubber->value());
    });
    connect(d->scrubber, &QSlider::valueChanged,
            this, &SessionPlayerW::on_scrub_moved);

    d->durationLabel = new QLabel(
        SessionInfo::ms_to_hms(d->durationMs) + ".000");
    d->durationLabel->setStyleSheet(
        "color:#44446a;font-size:14px;font-family:'Courier New';min-width:90px;");

    scrubRow->addWidget(d->timeLabel);
    scrubRow->addWidget(d->scrubber, 1);
    scrubRow->addWidget(d->durationLabel);
    root->addLayout(scrubRow);

    // ── Transport bar ──────────────────────────────────────────────────────
    auto* transport = new QHBoxLayout;
    transport->setSpacing(6);

    d->stepBackBtn = new QToolButton;
    d->stepBackBtn->setText("◀");
    d->stepBackBtn->setToolTip("Step back one frame (←)");
    connect(d->stepBackBtn, &QToolButton::clicked,
            this, [this]{ on_step(-1); });

    d->playBtn = new QPushButton("▶  Play");
    d->playBtn->setMinimumWidth(90);
    d->playBtn->setShortcut(Qt::Key_Space);
    connect(d->playBtn, &QPushButton::clicked,
            this, &SessionPlayerW::on_play_pause);

    d->stepFwdBtn = new QToolButton;
    d->stepFwdBtn->setText("▶");
    d->stepFwdBtn->setToolTip("Step forward one frame (→)");
    connect(d->stepFwdBtn, &QToolButton::clicked,
            this, [this]{ on_step(+1); });

    d->stopBtn = new QPushButton("■  Stop");
    connect(d->stopBtn, &QPushButton::clicked,
            this, &SessionPlayerW::on_stop);

    auto* speedLbl = new QLabel("Speed:");
    speedLbl->setStyleSheet("color:#44446a;font-size:10px;");
    d->speedBox = new QComboBox;
    d->speedBox->addItems({"0.25×", "0.5×", "1×", "2×", "4×"});
    d->speedBox->setCurrentIndex(2);
    d->speedBox->setFixedWidth(70);
    connect(d->speedBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SessionPlayerW::on_speed_changed);

    auto* volLbl = new QLabel("Vol:");
    volLbl->setStyleSheet("color:#44446a;font-size:10px;");
    d->volSlider = new QSlider(Qt::Horizontal);
    d->volSlider->setRange(0, 100);
    d->volSlider->setValue(80);
    d->volSlider->setFixedWidth(80);
    connect(d->volSlider, &QSlider::valueChanged,
            this, &SessionPlayerW::on_volume_changed);

    auto* hintLbl = new QLabel("Space play/pause  ·  ← → seek 2s  ·  [ ] speed");
    hintLbl->setStyleSheet("color:#2a2a44;font-size:9px;");

    transport->addWidget(d->stepBackBtn);
    transport->addWidget(d->playBtn);
    transport->addWidget(d->stepFwdBtn);
    transport->addWidget(d->stopBtn);
    transport->addSpacing(12);
    transport->addWidget(speedLbl);
    transport->addWidget(d->speedBox);
    transport->addSpacing(12);
    transport->addWidget(volLbl);
    transport->addWidget(d->volSlider);
    transport->addStretch();
    transport->addWidget(hintLbl);
    root->addLayout(transport);

    update_time_display(0);
}

// ── Player setup ──────────────────────────────────────────────────────────

void SessionPlayerW::setup_players()
{
    const int nCams = static_cast<int>(d->units.size());

    for (int i = 0; i < nCams; ++i) {
        auto& u = d->units[i];
        const QString videoPath = d->session.path + "/" + QString("video_%1.mp4").arg(i);

        if (!QFileInfo::exists(videoPath)) {
            u.hasVideo = false;
            u.slot->set_no_video();
            log_info(QString("[Player] Cam %1: no video file").arg(i));
            continue;
        }

        u.hasVideo = true;
        u.player   = new QMediaPlayer(this);
        u.sink     = new QVideoSink(this);

        u.player->setVideoOutput(u.sink);
        u.player->setSource(QUrl::fromLocalFile(videoPath));

        // Video-only — mute all video players; audio comes from audio.wav.
        auto* dummyAudio = new QAudioOutput(this);
        dummyAudio->setVolume(0.0f);
        u.player->setAudioOutput(dummyAudio);

        // Frame callback: update the camera slot's display label.
        const int camIdx = i;
        connect(u.sink, &QVideoSink::videoFrameChanged,
                this, [this, camIdx](const QVideoFrame& vf) {
            if (camIdx < d->units.size()) {
                d->units[camIdx].slot->update_frame(vf);
            }
        });
    }

    // ── Audio player ───────────────────────────────────────────────────────
    const QString audioPath = d->session.path + "/audio.wav";
    if (QFileInfo::exists(audioPath)) {
        d->audioPlayer = new QMediaPlayer(this);
        d->audioOutput = new QAudioOutput(this);
        d->audioOutput->setVolume(static_cast<float>(d->volSlider->value()) / 100.0f);
        d->audioPlayer->setAudioOutput(d->audioOutput);
        d->audioPlayer->setSource(QUrl::fromLocalFile(audioPath));
    }

    log_info(QString("[Player] %1 camera players set up.").arg(nCams));
}

// ── Playback control ──────────────────────────────────────────────────────

void SessionPlayerW::on_play_pause()
{
    if (d->playing) {
        // Pause
        d->masterMs = d->current_master_ms();
        d->playing  = false;
        for (auto& u : d->units) {
            if (u.player) { u.player->pause(); }
        }
        if (d->audioPlayer) { d->audioPlayer->pause(); }
        d->playBtn->setText("▶  Play");
    } else {
        // Play (or resume)
        d->playing    = true;
        d->masterBase = d->masterMs;
        d->masterClock.restart();
        for (auto& u : d->units) {
            if (u.player && u.hasVideo) { u.player->play(); }
        }
        if (d->audioPlayer) { d->audioPlayer->play(); }
        d->playBtn->setText("⏸  Pause");
    }
    apply_playback_state();
}

void SessionPlayerW::on_stop()
{
    d->playing  = false;
    d->masterMs = 0;
    for (auto& u : d->units) {
        if (u.player) { u.player->stop(); }
    }
    if (d->audioPlayer) { d->audioPlayer->stop(); }
    d->playBtn->setText("▶  Play");
    seek_all(0);
}

void SessionPlayerW::on_step(int direction)
{
    if (d->playing) { on_play_pause(); }   // pause first
    const int64_t stepMs = static_cast<int64_t>(
        std::round(1000.0 / d->manifest.master_fps()));
    const int64_t newPos = std::clamp(
        d->masterMs + direction * stepMs, 0LL, d->durationMs);
    seek_all(newPos);
}

void SessionPlayerW::on_scrub_moved(int valueMs)
{
    if (!d->scrubbing) { return; }
    // Update time label immediately while dragging (don't seek yet).
    update_time_display(valueMs);
    d->annotBar->set_position(valueMs);
}

void SessionPlayerW::on_speed_changed(int index)
{
    static constexpr double rates[] = {0.25, 0.5, 1.0, 2.0, 4.0};
    if (index < 0 || index >= 5) { return; }
    const int64_t curPos = d->current_master_ms();
    d->rate       = rates[index];
    d->masterMs   = curPos;
    d->masterBase = curPos;
    if (d->playing) { d->masterClock.restart(); }
    for (auto& u : d->units) {
        if (u.player) { u.player->setPlaybackRate(d->rate); }
    }
    if (d->audioPlayer) { d->audioPlayer->setPlaybackRate(d->rate); }
}

void SessionPlayerW::on_volume_changed(int value)
{
    if (d->audioOutput) {
        d->audioOutput->setVolume(static_cast<float>(value) / 100.0f);
    }
}

// ── Seek ─────────────────────────────────────────────────────────────────

void SessionPlayerW::seek_all(int64_t masterMs)
{
    masterMs = std::clamp(masterMs, 0LL, d->durationMs);

    d->masterMs   = masterMs;
    d->masterBase = masterMs;
    if (d->playing) { d->masterClock.restart(); }

    for (auto& u : d->units) {
        if (!u.player || !u.hasVideo) { continue; }
        const int64_t videoPos = masterMs + u.seekOffsetMs;
        u.player->setPosition(std::max(0LL, videoPos));
    }

    if (d->audioPlayer) {
        const int64_t audioPos = masterMs + d->manifest.audio_seek_ms();
        d->audioPlayer->setPosition(std::max(0LL, audioPos));
    }

    if (!d->scrubbing) {
        d->scrubber->blockSignals(true);
        d->scrubber->setValue(static_cast<int>(masterMs));
        d->scrubber->blockSignals(false);
    }

    update_time_display(masterMs);
    d->annotBar->set_position(masterMs);
}

// ── Periodic timers ───────────────────────────────────────────────────────

void SessionPlayerW::on_master_timer_tick()
{
    if (!d->playing || d->scrubbing) { return; }

    const int64_t pos = d->current_master_ms();
    if (pos >= d->durationMs) {
        on_stop();
        return;
    }

    // Update scrubber and time without triggering seek.
    d->scrubber->blockSignals(true);
    d->scrubber->setValue(static_cast<int>(pos));
    d->scrubber->blockSignals(false);
    update_time_display(pos);
    d->annotBar->set_position(pos);
}

void SessionPlayerW::on_sync_tick()
{
    // Drift correction: if any video player has drifted more than 300 ms,
    // re-seek it.  Only do this while actively playing.
    if (!d->playing) { return; }

    const int64_t masterPos = d->current_master_ms();
    for (auto& u : d->units) {
        if (!u.player || !u.hasVideo) { continue; }
        const int64_t expected = masterPos + u.seekOffsetMs;
        const int64_t actual   = u.player->position();
        if (std::abs(expected - actual) > 300) {
            u.player->setPosition(std::max(0LL, expected));
        }
    }
}

// ── UI helpers ────────────────────────────────────────────────────────────

void SessionPlayerW::apply_playback_state()
{
    d->stepBackBtn->setEnabled(!d->playing);
    d->stepFwdBtn ->setEnabled(!d->playing);
}

void SessionPlayerW::update_time_display(int64_t masterMs)
{
    const int64_t h   =  masterMs / 3600000LL;
    const int64_t m   = (masterMs % 3600000LL) / 60000LL;
    const int64_t s   = (masterMs % 60000LL)   / 1000LL;
    const int64_t ms  =  masterMs % 1000LL;
    d->timeLabel->setText(
        QString("%1:%2:%3.%4")
            .arg(h, 2, 10, QChar('0'))
            .arg(m, 2, 10, QChar('0'))
            .arg(s, 2, 10, QChar('0'))
            .arg(ms, 3, 10, QChar('0')));
}

void SessionPlayerW::update_camera_overlays()
{
    if (!d->manifest.is_valid()) { return; }
    for (int i = 0; i < d->manifest.camera_count() && i < d->units.size(); ++i) {
        const auto& cs = d->manifest.camera_info(i);
        d->units[i].slot->set_stats(cs.fpsActual, cs.coveragePct);
    }
}

// ── Keyboard shortcuts ────────────────────────────────────────────────────

void SessionPlayerW::keyPressEvent(QKeyEvent* e)
{
    switch (e->key()) {
    case Qt::Key_Space:
        on_play_pause();
        break;
    case Qt::Key_Left:
        on_step(-1);
        if (e->modifiers() & Qt::ShiftModifier) { seek_all(d->masterMs - 2000); }
        else                                     { seek_all(d->masterMs - 2000); }
        break;
    case Qt::Key_Right:
        seek_all(d->masterMs + 2000);
        break;
    case Qt::Key_BracketLeft:
    {
        int idx = d->speedBox->currentIndex();
        if (idx > 0) { d->speedBox->setCurrentIndex(idx - 1); }
        break;
    }
    case Qt::Key_BracketRight:
    {
        int idx = d->speedBox->currentIndex();
        if (idx < d->speedBox->count() - 1) { d->speedBox->setCurrentIndex(idx + 1); }
        break;
    }
    case Qt::Key_Home:
        seek_all(0);
        break;
    case Qt::Key_End:
        seek_all(d->durationMs);
        break;
    default:
        QDialog::keyPressEvent(e);
    }
}

void SessionPlayerW::resizeEvent(QResizeEvent* e)
{
    QDialog::resizeEvent(e);
}

} // namespace mosaic

#include "session_player_w.moc"
