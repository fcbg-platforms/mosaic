#include "ui/calibration/room_calibration_w.hpp"
#include "video/video_manager.hpp"
#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace mosaic {

struct RoomCalibrationW::Impl {
    VideoSettings&         videoSettings;
    RoomSettings&          roomSettings;
    VideoManager*          videoMgr;
    RoomCalibrationManager manager;

    // Board configuration
    QSpinBox*       colsSpin   = nullptr;
    QSpinBox*       rowsSpin   = nullptr;
    QDoubleSpinBox* squareSpin = nullptr;
    QDoubleSpinBox* markerSpin = nullptr;

    // Per-camera thumbnails + last-shot found dots (index = camera index)
    QVector<QLabel*> thumbLabels;
    QVector<QLabel*> foundDots;

    // Capture
    QPushButton*        captureBtn   = nullptr;
    QLabel*              shotCountLbl = nullptr;
    QTimer                captureTimer;
    // Extra grace period after captureTimer closes a shot's acceptance
    // window, before the button re-enables a NEW capture — purely a UX
    // pacing choice (avoids the button flickering enabled/disabled right at
    // the 500ms edge). Correctness against a late-arriving reply no longer
    // depends on this timing at all: on_calibration_frame_ready() rejects
    // any reply whose echoed token doesn't match currentShotToken, so a
    // frame delayed past this whole window (e.g. GigE resend) is dropped
    // rather than misattributed to whichever shot happens to be active when
    // it finally arrives.
    QTimer                cooldownTimer;
    bool                  awaitingShot  = false;
    QVector<VideoFrame>   pendingFrames;
    int                   lastShotIndex = -1;
    // Incremented once per capture_shot() call and passed to every
    // request_calibration_frame() call for that shot. on_calibration_frame_ready()
    // only accepts a reply whose echoed token matches the CURRENT value —
    // the structural fix for the race the awaitingShot/timer dance above can
    // only narrow, not close: a reply delayed past this shot's whole
    // capture+cooldown window (e.g. GigE resend) would otherwise still be
    // silently misattributed to whatever shot is active when it finally
    // arrives.
    uint64_t              currentShotToken = 0;

    // Solve
    QComboBox*    referenceCombo = nullptr;
    QPushButton*  solveBtn       = nullptr;
    QTableWidget* resultTable    = nullptr;

    // Plane + save
    QPushButton*           usePlaneBtn    = nullptr;
    QPushButton*           saveBtn        = nullptr;
    QLabel*                planeStatusLbl = nullptr;
    bool                   pendingPlane   = false;
    std::array<double, 3>  pendingPlanePoint  = {0, 0, 0};
    std::array<double, 3>  pendingPlaneNormal = {0, 0, 1};

    explicit Impl(VideoSettings& vs, RoomSettings& rs, VideoManager* vm)
        : videoSettings(vs), roomSettings(rs), videoMgr(vm) {}
};

RoomCalibrationW::RoomCalibrationW(VideoSettings& videoSettings, RoomSettings& roomSettings,
                                    VideoManager* videoMgr, QWidget* parent)
    : QWidget(parent), d(std::make_unique<Impl>(videoSettings, roomSettings, videoMgr))
{
    d->captureTimer.setSingleShot(true);
    d->cooldownTimer.setSingleShot(true);

    auto* outerLay = new QVBoxLayout(this);
    outerLay->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content    = new QWidget;
    auto* contentLay = new QVBoxLayout(content);
    contentLay->setContentsMargins(10, 10, 10, 10);
    contentLay->setSpacing(10);

    if (!RoomCalibrationManager::is_available() || !d->videoMgr) {
        auto* note = new QLabel(
            "Room (extrinsic) calibration requires OpenCV and an active video manager.\n"
            "Rebuild with -DMOSAIC_ENABLE_OPENCV=ON.");
        note->setProperty("role", "muted");
        note->setAlignment(Qt::AlignCenter);
        note->setWordWrap(true);
        contentLay->addWidget(note);
        contentLay->addStretch();
        scroll->setWidget(content);
        outerLay->addWidget(scroll);
        return;
    }

    build_board_section(contentLay);
    build_camera_section(contentLay);
    build_capture_section(contentLay);
    contentLay->addStretch();

    scroll->setWidget(content);
    outerLay->addWidget(scroll);

    rebuild_board_spec();
    refresh_intrinsics();

    connect(d->videoMgr, &VideoManager::calibration_frame_ready,
            this,         &RoomCalibrationW::on_calibration_frame_ready);
    connect(d->videoMgr, &VideoManager::frame_preview,
            this,         &RoomCalibrationW::on_preview_frame);
    connect(&d->captureTimer, &QTimer::timeout, this, &RoomCalibrationW::finalize_shot);
    connect(&d->cooldownTimer, &QTimer::timeout, this, [this] {
        d->captureBtn->setEnabled(true);
        d->captureBtn->setText("▶  Capture shot");
    });
}

RoomCalibrationW::~RoomCalibrationW() = default;

void RoomCalibrationW::refresh_intrinsics() {
    for (int i = 0; i < static_cast<int>(d->videoSettings.cameras.size()); ++i) {
        d->manager.set_camera_intrinsics(i, d->videoSettings.cameras[static_cast<size_t>(i)].calibration);
    }
}

// ── Board section ──────────────────────────────────────────────────────────

void RoomCalibrationW::build_board_section(QVBoxLayout* parent) {
    auto* box = new QGroupBox("ChArUco board");
    auto* row = new QHBoxLayout(box);
    row->setSpacing(12);

    auto make_label = [](const QString& txt) {
        auto* lbl = new QLabel(txt);
        lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return lbl;
    };

    row->addWidget(make_label("Cols:"));
    d->colsSpin = new QSpinBox;
    d->colsSpin->setRange(3, 20);
    d->colsSpin->setValue(7);
    d->colsSpin->setFixedWidth(55);
    row->addWidget(d->colsSpin);

    row->addSpacing(8);
    row->addWidget(make_label("Rows:"));
    d->rowsSpin = new QSpinBox;
    d->rowsSpin->setRange(3, 20);
    d->rowsSpin->setValue(5);
    d->rowsSpin->setFixedWidth(55);
    row->addWidget(d->rowsSpin);

    row->addSpacing(8);
    row->addWidget(make_label("Square:"));
    d->squareSpin = new QDoubleSpinBox;
    d->squareSpin->setRange(1.0, 500.0);
    d->squareSpin->setValue(40.0);
    d->squareSpin->setSuffix("  mm");
    d->squareSpin->setFixedWidth(95);
    row->addWidget(d->squareSpin);

    row->addSpacing(8);
    row->addWidget(make_label("Marker:"));
    d->markerSpin = new QDoubleSpinBox;
    d->markerSpin->setRange(1.0, 500.0);
    d->markerSpin->setValue(30.0);
    d->markerSpin->setSuffix("  mm");
    d->markerSpin->setFixedWidth(95);
    row->addWidget(d->markerSpin);
    row->addStretch();

    for (auto* spin : {d->colsSpin, d->rowsSpin}) {
        connect(spin, &QSpinBox::valueChanged, this, &RoomCalibrationW::rebuild_board_spec);
    }
    for (auto* spin : {d->squareSpin, d->markerSpin}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, &RoomCalibrationW::rebuild_board_spec);
    }

    parent->addWidget(box);
}

void RoomCalibrationW::rebuild_board_spec() {
    RoomCalibrationManager::BoardSpec spec;
    spec.cols           = d->colsSpin->value();
    spec.rows           = d->rowsSpin->value();
    spec.squareLengthMm = d->squareSpin->value();
    spec.markerLengthMm = d->markerSpin->value();
    d->manager.set_board(spec);
}

// ── Camera thumbnails ────────────────────────────────────────────────────────

void RoomCalibrationW::build_camera_section(QVBoxLayout* parent) {
    auto* box = new QGroupBox("Cameras");
    auto* row = new QHBoxLayout(box);

    const int n = static_cast<int>(d->videoSettings.cameras.size());
    d->thumbLabels.resize(n);
    d->foundDots.resize(n);

    for (int i = 0; i < n; ++i) {
        auto* col = new QVBoxLayout;

        auto* thumb = new QLabel("(no frame)");
        thumb->setAlignment(Qt::AlignCenter);
        thumb->setFixedSize(120, 68);
        thumb->setStyleSheet("QLabel { background: #09090f; border-radius: 4px; "
                              "color: #404060; font-size: 10px; }");
        d->thumbLabels[i] = thumb;
        col->addWidget(thumb);

        auto* caption = new QHBoxLayout;
        caption->addWidget(new QLabel(QString("Cam %1").arg(i)));
        auto* dot = new QLabel("○");
        dot->setStyleSheet("color: #606080;");
        d->foundDots[i] = dot;
        caption->addWidget(dot);
        col->addLayout(caption);

        row->addLayout(col);
    }
    if (n == 0) { row->addWidget(new QLabel("No cameras configured")); }
    row->addStretch();

    parent->addWidget(box);
}

void RoomCalibrationW::on_preview_frame(int cameraIndex, QImage frame) {
    // This widget is created once at startup (a permanent settings tab, not
    // built lazily on first selection — see main_window.cpp), so without
    // this guard every camera's preview frame would pay a QPixmap
    // conversion + scale on the GUI thread for the entire app lifetime,
    // even while a completely different tab is showing. isVisible()
    // correctly reflects both tab levels (this page inside CalibrationW's
    // inner QTabWidget, which is itself inside the outer settings
    // QTabWidget) plus the main window's own visibility.
    if (!isVisible()) { return; }
    if (cameraIndex < 0 || cameraIndex >= d->thumbLabels.size()) { return; }
    d->thumbLabels[cameraIndex]->setPixmap(
        QPixmap::fromImage(frame).scaled(120, 68, Qt::KeepAspectRatio, Qt::FastTransformation));
}

// ── Capture section ────────────────────────────────────────────────────────

void RoomCalibrationW::build_capture_section(QVBoxLayout* parent) {
    auto* box = new QGroupBox("Capture");
    auto* vlay = new QVBoxLayout(box);

    auto* row1 = new QHBoxLayout;
    d->captureBtn = new QPushButton("▶  Capture shot");
    connect(d->captureBtn, &QPushButton::clicked, this, &RoomCalibrationW::capture_shot);
    row1->addWidget(d->captureBtn);

    d->shotCountLbl = new QLabel("Shots: 0");
    d->shotCountLbl->setProperty("role", "muted");
    row1->addWidget(d->shotCountLbl);
    row1->addStretch();
    vlay->addLayout(row1);

    auto* note = new QLabel(
        "Move the board through overlapping pairs of camera views, capturing a shot each time "
        "it's visible to at least two cameras. Camera 0 is the fixed room-origin reference.");
    note->setProperty("role", "muted");
    note->setWordWrap(true);
    vlay->addWidget(note);

    d->resultTable = new QTableWidget(0, 3);
    d->resultTable->setHorizontalHeaderLabels({"Camera", "Resolved", "Reprojection RMS (px)"});
    d->resultTable->horizontalHeader()->setStretchLastSection(true);
    d->resultTable->verticalHeader()->setVisible(false);
    d->resultTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    d->resultTable->setSelectionMode(QAbstractItemView::NoSelection);
    d->resultTable->setMaximumHeight(160);
    vlay->addWidget(d->resultTable);

    auto* row2 = new QHBoxLayout;
    row2->addWidget(new QLabel("Reference camera:"));
    d->referenceCombo = new QComboBox;
    for (int i = 0; i < static_cast<int>(d->videoSettings.cameras.size()); ++i) {
        d->referenceCombo->addItem(QString("Camera %1").arg(i));
    }
    row2->addWidget(d->referenceCombo);

    d->solveBtn = new QPushButton("▶  Solve");
    connect(d->solveBtn, &QPushButton::clicked, this, &RoomCalibrationW::solve);
    row2->addWidget(d->solveBtn);
    row2->addStretch();
    vlay->addLayout(row2);

    auto* row3 = new QHBoxLayout;
    d->usePlaneBtn = new QPushButton("Use last shot as plane");
    d->usePlaneBtn->setEnabled(false);
    connect(d->usePlaneBtn, &QPushButton::clicked, this, &RoomCalibrationW::use_shot_as_plane);
    row3->addWidget(d->usePlaneBtn);

    d->saveBtn = new QPushButton("Save to settings");
    d->saveBtn->setEnabled(false);
    connect(d->saveBtn, &QPushButton::clicked, this, &RoomCalibrationW::save_to_settings);
    row3->addWidget(d->saveBtn);
    row3->addStretch();
    vlay->addLayout(row3);

    d->planeStatusLbl = new QLabel("Plane: not defined");
    d->planeStatusLbl->setProperty("role", "muted");
    d->planeStatusLbl->setWordWrap(true);
    vlay->addWidget(d->planeStatusLbl);

    parent->addWidget(box);
}

// ── Capture flow ──────────────────────────────────────────────────────────

void RoomCalibrationW::capture_shot() {
    if (d->awaitingShot) { return; }
    d->awaitingShot = true;
    ++d->currentShotToken;
    d->pendingFrames.clear();
    d->captureBtn->setEnabled(false);
    d->captureBtn->setText("Capturing…");

    const int n = static_cast<int>(d->videoSettings.cameras.size());
    for (int i = 0; i < n; ++i) {
        d->videoMgr->request_calibration_frame(i, d->currentShotToken);
    }
    d->captureTimer.start(500);
}

void RoomCalibrationW::on_calibration_frame_ready(int cameraIndex, QImage frame, uint64_t token) {
    if (!d->awaitingShot || frame.isNull() || token != d->currentShotToken) { return; }

    const QImage bgr = (frame.format() == QImage::Format_BGR888)
        ? frame
        : frame.convertToFormat(QImage::Format_BGR888);

    VideoFrame vf;
    vf.cameraIndex = cameraIndex;
    vf.width       = bgr.width();
    vf.height      = bgr.height();
    vf.stride      = static_cast<int>(bgr.bytesPerLine());
    vf.data.assign(bgr.constBits(), bgr.constBits() + static_cast<size_t>(bgr.sizeInBytes()));
    d->pendingFrames.push_back(std::move(vf));
}

void RoomCalibrationW::finalize_shot() {
    d->awaitingShot = false;
    // Keep the button disabled a bit longer (see cooldownTimer's doc
    // comment in Impl) so a stale in-flight frame from this shot has time
    // to arrive and be correctly ignored (awaitingShot is already false)
    // before a new shot can start accepting frames.
    d->captureBtn->setText("Settling…");
    d->cooldownTimer.start(250);

    const int shotsBefore = d->manager.shot_count();
    const auto results = d->manager.feed_shot(d->pendingFrames);
    d->pendingFrames.clear();

    for (const auto& r : results) {
        if (r.cameraIndex < 0 || r.cameraIndex >= d->foundDots.size()) { continue; }
        d->foundDots[r.cameraIndex]->setText(r.found ? "●" : "○");
        d->foundDots[r.cameraIndex]->setStyleSheet(
            r.found ? "color: #44cc44;" : "color: #cc4444;");
        d->foundDots[r.cameraIndex]->setToolTip(
            r.found ? QString("%1 ChArUco corners found").arg(r.cornerCount)
                    : "No usable board detection in this shot.");
    }

    if (d->manager.shot_count() > shotsBefore) {
        d->lastShotIndex = d->manager.shot_count() - 1;
    }
    d->shotCountLbl->setText(QString("Shots: %1").arg(d->manager.shot_count()));
}

// ── Solve / plane / save ────────────────────────────────────────────────────

void RoomCalibrationW::solve() {
    const int cameraCount = static_cast<int>(d->videoSettings.cameras.size());
    if (cameraCount == 0) { return; }

    const int refIdx = d->referenceCombo->currentIndex();
    d->manager.solve(cameraCount, refIdx < 0 ? 0 : refIdx);
    update_result_table();

    const bool anyResolved = [&] {
        for (int i = 0; i < cameraCount; ++i) {
            if (d->manager.is_resolved(i)) { return true; }
        }
        return false;
    }();
    d->saveBtn->setEnabled(anyResolved);
    d->usePlaneBtn->setEnabled(anyResolved && d->lastShotIndex >= 0);
}

void RoomCalibrationW::update_result_table() {
    const int n = static_cast<int>(d->videoSettings.cameras.size());
    d->resultTable->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        const bool   resolved = d->manager.is_resolved(i);
        const double rms      = d->manager.reprojection_rms_for(i);

        auto* camItem = new QTableWidgetItem(QString("Camera %1").arg(i));
        auto* resItem = new QTableWidgetItem(resolved ? "Yes" : "No");
        auto* rmsItem = new QTableWidgetItem(rms >= 0.0 ? QString::number(rms, 'f', 3) : "—");
        if (!resolved) {
            resItem->setForeground(QColor("#cc4444"));
        }
        d->resultTable->setItem(i, 0, camItem);
        d->resultTable->setItem(i, 1, resItem);
        d->resultTable->setItem(i, 2, rmsItem);
    }
}

void RoomCalibrationW::use_shot_as_plane() {
    if (d->lastShotIndex < 0) { return; }

    std::array<double, 3> point{}, normal{};
    bool ok = false;
    for (int i = 0; i < static_cast<int>(d->videoSettings.cameras.size()); ++i) {
        if (d->manager.use_shot_as_plane(d->lastShotIndex, i, point, normal)) {
            ok = true;
            break;
        }
    }

    if (!ok) {
        QMessageBox::warning(this, "No plane available",
            "The last captured shot has no detection from an already-resolved camera. "
            "Run Solve first, then capture (or re-use) a shot with the board flat on the "
            "target surface.");
        return;
    }

    d->pendingPlane       = true;
    d->pendingPlanePoint  = point;
    d->pendingPlaneNormal = normal;
    d->planeStatusLbl->setText(
        QString("Plane candidate ready — point (%1, %2, %3) mm. Click \"Save to settings\" to persist.")
            .arg(point[0], 0, 'f', 1).arg(point[1], 0, 'f', 1).arg(point[2], 0, 'f', 1));
}

void RoomCalibrationW::save_to_settings() {
    int savedCount = 0;
    int clearedCount = 0;
    for (int i = 0; i < static_cast<int>(d->videoSettings.cameras.size()); ++i) {
        auto& cal = d->videoSettings.cameras[static_cast<size_t>(i)].calibration;
        if (d->manager.is_resolved(i)) {
            cal.extrinsicRt         = d->manager.extrinsic_for(i);
            cal.extrinsicCalibrated = true;
            ++savedCount;
        } else if (cal.extrinsicCalibrated) {
            // This camera resolved in an earlier run (extrinsicCalibrated
            // was already true) but did NOT resolve this time — e.g. the
            // rig was physically adjusted and this camera no longer shares
            // a shot with anything. Clear the stale flag rather than
            // silently leaving a now-unverified pose marked as valid;
            // run_gaze_fusion.py trusts extrinsicCalibrated at face value
            // and would otherwise keep fusing a systematically wrong ray
            // from this camera with no indication anything changed.
            cal.extrinsicCalibrated = false;
            ++clearedCount;
        }
    }

    if (d->pendingPlane) {
        d->roomSettings.planePoint   = d->pendingPlanePoint;
        d->roomSettings.planeNormal  = d->pendingPlaneNormal;
        d->roomSettings.planeDefined = true;
    }

    emit extrinsics_saved();

    QString msg = QString("Extrinsics stored for %1 camera(s).%2")
        .arg(savedCount)
        .arg(d->pendingPlane ? "\nRoom plane also stored." : "");
    if (clearedCount > 0) {
        msg += QString("\n%1 camera(s) previously calibrated no longer resolved and had "
                        "their extrinsics cleared — re-run Solve with more overlapping shots "
                        "to restore them.").arg(clearedCount);
    }
    msg += "\nWritten to the settings file on application exit.";
    QMessageBox::information(this, "Room calibration saved", msg);
}

} // namespace mosaic
