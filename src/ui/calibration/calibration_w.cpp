#include "ui/calibration/calibration_w.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "calibration/rms_quality.hpp"
#include "ui/calibration/badge_style.hpp"
#include "ui/calibration/room_calibration_w.hpp"
#include "utils/logger.hpp"
#include "video/video_manager.hpp"

namespace mosaic {

struct CalibrationW::Impl {
    VideoSettings& videoSettings;
    CalibrationManager manager;
    RoomCalibrationW* roomTab = nullptr; // not owned (child widget, Qt parent-owns)
    VideoManager* videoMgr    = nullptr; // not owned
    QTabWidget* innerTabs     = nullptr; // not owned — Intrinsics is index 0

    // Board configuration
    QSpinBox* colsSpin         = nullptr;
    QSpinBox* rowsSpin         = nullptr;
    QDoubleSpinBox* squareSpin = nullptr;

    // Capture controls
    QComboBox* cameraCombo = nullptr;
    QLabel* viewCountLabel = nullptr;

    // Preview
    QLabel* previewLabel = nullptr;

    // Result
    QLabel* rmsLabel          = nullptr;
    QLabel* fxLabel           = nullptr;
    QLabel* fyLabel           = nullptr;
    QLabel* cxLabel           = nullptr;
    QLabel* cyLabel           = nullptr;
    QPushButton* saveBtn      = nullptr;
    QPushButton* calibrateBtn = nullptr;

    explicit Impl(VideoSettings& vs) : videoSettings(vs) {}
};

CalibrationW::CalibrationW(VideoSettings& videoSettings, RoomSettings& roomSettings,
                           VideoManager* videoMgr, QWidget* parent)
    : QWidget(parent), d(std::make_unique<Impl>(videoSettings)) {
    d->videoMgr = videoMgr;

    auto* outerLay = new QVBoxLayout(this);
    outerLay->setContentsMargins(0, 0, 0, 0);

    auto* innerTabs = new QTabWidget;
    innerTabs->setDocumentMode(true);
    outerLay->addWidget(innerTabs);
    d->innerTabs = innerTabs;

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content    = new QWidget;
    auto* contentLay = new QVBoxLayout(content);
    contentLay->setContentsMargins(10, 10, 10, 10);
    contentLay->setSpacing(10);

    if (!CalibrationManager::is_available()) {
        auto* note = new QLabel(
            "Camera calibration requires OpenCV.\n"
            "Rebuild with -DMOSAIC_HAVE_OPENCV=ON.");
        note->setProperty("role", "muted");
        note->setAlignment(Qt::AlignCenter);
        note->setWordWrap(true);
        contentLay->addWidget(note);
        contentLay->addStretch();
        scroll->setWidget(content);
        innerTabs->addTab(scroll, "Intrinsics");
        d->roomTab = new RoomCalibrationW(videoSettings, roomSettings, videoMgr);
        innerTabs->addTab(d->roomTab, "Room (Extrinsics)");
        return;
    }

    build_board_section(contentLay);

    // Keep CalibrationManager::set_board() synced with the spinboxes at all
    // times, not just when "Calibrate" is clicked — feed_frame() (see the
    // capture-frame connection below) bakes the *currently-set* board's
    // squareSizeMm into every accepted view's 3-D object points as it's
    // fed, not retroactively at calibrate() time. Previously set_board()
    // only ran inside the Calibrate button's click handler, by which point
    // every already-accumulated view had silently used CalibrationManager's
    // own internal default (25mm) regardless of what the spinbox showed —
    // a real correctness bug (wrong absolute scale in the result), not
    // just a cosmetic mismatch, whenever a user changed Square away from
    // that default before capturing.
    const auto sync_board_spec = [this] {
        CalibrationManager::BoardSpec spec;
        spec.cols         = d->colsSpin->value();
        spec.rows         = d->rowsSpin->value();
        spec.squareSizeMm = d->squareSpin->value();
        d->manager.set_board(spec);
    };
    sync_board_spec();
    connect(d->colsSpin, qOverload<int>(&QSpinBox::valueChanged), this, sync_board_spec);
    connect(d->rowsSpin, qOverload<int>(&QSpinBox::valueChanged), this, sync_board_spec);
    connect(d->squareSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, sync_board_spec);

    build_capture_section(contentLay);
    build_preview_section(contentLay);
    build_result_section(contentLay);
    contentLay->addStretch();

    scroll->setWidget(content);
    innerTabs->addTab(scroll, "Intrinsics");

    d->roomTab = new RoomCalibrationW(videoSettings, roomSettings, videoMgr);
    innerTabs->addTab(d->roomTab, "Room (Extrinsics)");

    // Connect CalibrationManager signals.
    connect(&d->manager, &CalibrationManager::corners_detected, this,
            &CalibrationW::on_corners_detected);
    connect(&d->manager, &CalibrationManager::calibration_done, this,
            &CalibrationW::on_calibration_done);
    connect(&d->manager, &CalibrationManager::calibration_started, this, [this] {
        d->calibrateBtn->setEnabled(false);
        d->calibrateBtn->setText("Calibrating…");
    });

    // A fresh intrinsic calibration must be visible to the Room tab's
    // solvePnP-based extrinsic solve immediately, not just after this
    // widget is recreated.
    connect(this, &CalibrationW::calibration_saved, d->roomTab,
            &RoomCalibrationW::refresh_intrinsics);

    // Feed live full-resolution frames from whichever camera is selected in
    // the capture section into the calibration manager — previously nothing
    // in the app ever called CalibrationManager::feed_frame() at all (the
    // "Capture" section's own tip label described this as something the
    // caller must wire up, but no caller ever did), so "Views accepted"
    // could never advance past 0 no matter how the checkerboard was shown
    // to the camera.
    //
    // Deliberately reuses VideoManager::request_calibration_frame()/
    // calibration_frame_ready() — the same one-shot full-resolution
    // mechanism RoomCalibrationW's "Capture Shot" button already uses —
    // polled on a timer here instead of a single click, rather than feeding
    // VideoManager::frame_preview() directly: that signal is capped to
    // 640x360 for cheap UI display (VideoGrabber::run_pylon_loop()'s own
    // comment), and at that size a real printed checkerboard held at a
    // normal distance shrinks to only a few pixels per square — nowhere
    // near enough for cv::findChessboardCorners to reliably detect,
    // confirmed on real hardware: feed_frame() ran continuously against
    // the downscaled preview and never found the pattern once. Full
    // resolution (1920x1080 on this rig) gives the detector real pixels to
    // work with.
    if (d->videoMgr) {
        connect(
            d->videoMgr, &VideoManager::calibration_frame_ready, this,
            [this](int camIdx, QImage frame, uint64_t /*token*/) {
                if (camIdx != d->cameraCombo->currentIndex()) {
                    return;
                }

                if (frame.format() != QImage::Format_BGR888) {
                    frame = frame.convertToFormat(QImage::Format_BGR888);
                }
                VideoFrame vf;
                vf.width  = frame.width();
                vf.height = frame.height();
                vf.stride = static_cast<int>(frame.bytesPerLine());
                vf.data.assign(frame.constBits(),
                               frame.constBits() + static_cast<size_t>(vf.stride) * vf.height);
                log_info(QString("[CalibrationW] Feeding camera %1 full-res frame (%2x%3) into "
                                 "intrinsics calibration.")
                             .arg(camIdx)
                             .arg(vf.width)
                             .arg(vf.height));
                d->manager.feed_frame(vf);
            },
            Qt::QueuedConnection);

        // ~1.25 requests/sec — slow enough that cv::findChessboardCorners
        // (real synchronous CPU work on this, the GUI, thread) never stacks
        // up, and slow enough to naturally encourage moving the board
        // between accepted views instead of racking up near-duplicates.
        //
        // Gated on this widget's own visibility AND the Intrinsics sub-tab
        // specifically being selected — CalibrationW is a permanently-
        // instantiated top-level tab (constructed once at app startup, like
        // every other MainWindow tab), so without this check the timer would
        // fire full-resolution capture + synchronous OpenCV corner-search
        // forever, regardless of which tab is actually on screen, stalling
        // the GUI thread and visibly lagging live video everywhere in the
        // app — confirmed on real hardware. Mirrors the same
        // "gate expensive per-frame work on isVisible()" precedent
        // RoomCalibrationW's own live preview conversion already uses.
        auto* requestTimer = new QTimer(this);
        requestTimer->setInterval(800);
        connect(requestTimer, &QTimer::timeout, this, [this] {
            if (!isVisible()) {
                return;
            }
            if (d->innerTabs && d->innerTabs->currentIndex() != 0) {
                return;
            }
            if (d->cameraCombo->currentIndex() >= 0) {
                d->videoMgr->request_calibration_frame(d->cameraCombo->currentIndex());
            }
        });
        requestTimer->start();
    }
}

CalibrationW::~CalibrationW() = default;

// ── Board section ──────────────────────────────────────────────────────────

void CalibrationW::build_board_section(QVBoxLayout* parent) {
    auto* box = new QGroupBox("Checkerboard");
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
    d->colsSpin->setValue(9);
    d->colsSpin->setFixedWidth(55);
    row->addWidget(d->colsSpin);

    row->addSpacing(8);
    row->addWidget(make_label("Rows:"));
    d->rowsSpin = new QSpinBox;
    d->rowsSpin->setRange(3, 20);
    d->rowsSpin->setValue(6);
    d->rowsSpin->setFixedWidth(55);
    row->addWidget(d->rowsSpin);

    row->addSpacing(8);
    row->addWidget(make_label("Square:"));
    d->squareSpin = new QDoubleSpinBox;
    d->squareSpin->setRange(1.0, 500.0);
    // Matches the generated, print-ready checkerboard's actual physical
    // square size (9x6 inner corners, 22mm squares) — was 25mm, a mismatch
    // with the size the printed board is now generated at. Cols/Rows
    // already defaulted to 9/6, matching correctly.
    d->squareSpin->setValue(22.0);
    d->squareSpin->setSuffix("  mm");
    d->squareSpin->setFixedWidth(95);
    row->addWidget(d->squareSpin);
    row->addStretch();

    parent->addWidget(box);
}

// ── Capture section ────────────────────────────────────────────────────────

void CalibrationW::build_capture_section(QVBoxLayout* parent) {
    auto* box  = new QGroupBox("Capture");
    auto* vlay = new QVBoxLayout(box);

    auto* row1 = new QHBoxLayout;

    row1->addWidget(new QLabel("Camera:"));
    d->cameraCombo = new QComboBox;
    for (int i = 0; i < static_cast<int>(d->videoSettings.cameras.size()); ++i) {
        const QString name = d->videoSettings.cameras[static_cast<size_t>(i)].friendlyName;
        d->cameraCombo->addItem(QString("Camera %1 — %2").arg(i).arg(name));
    }
    if (d->cameraCombo->count() == 0) {
        d->cameraCombo->addItem("No cameras configured");
    }
    row1->addWidget(d->cameraCombo, 1);
    vlay->addLayout(row1);

    auto* row2        = new QHBoxLayout;
    d->viewCountLabel = new QLabel("Views accepted: 0");
    d->viewCountLabel->setProperty("role", "muted");
    row2->addWidget(d->viewCountLabel, 1);

    auto* clearBtn = new QPushButton("Clear views");
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        d->manager.clear_views();
        d->viewCountLabel->setText("Views accepted: 0");
        d->previewLabel->clear();
        d->previewLabel->setText("(no preview)");
        update_result_labels();
    });
    row2->addWidget(clearBtn);
    vlay->addLayout(row2);

    auto* noteLabel = new QLabel(
        "Tip: Supply frames to the calibration manager from the video grabber "
        "by calling CalibrationManager::feed_frame() as frames arrive.");
    noteLabel->setProperty("role", "muted");
    noteLabel->setWordWrap(true);
    vlay->addWidget(noteLabel);

    parent->addWidget(box);
}

// ── Preview section ────────────────────────────────────────────────────────

void CalibrationW::build_preview_section(QVBoxLayout* parent) {
    auto* box  = new QGroupBox("Last frame preview");
    auto* vlay = new QVBoxLayout(box);

    d->previewLabel = new QLabel("(no preview)");
    d->previewLabel->setAlignment(Qt::AlignCenter);
    d->previewLabel->setMinimumHeight(160);
    d->previewLabel->setStyleSheet(
        "QLabel { background: #09090f; border-radius: 4px; "
        "color: #404060; font-size: 11px; }");
    vlay->addWidget(d->previewLabel);

    parent->addWidget(box);
}

// ── Result section ─────────────────────────────────────────────────────────

void CalibrationW::build_result_section(QVBoxLayout* parent) {
    auto* box  = new QGroupBox("Result");
    auto* vlay = new QVBoxLayout(box);

    auto* row1      = new QHBoxLayout;
    d->calibrateBtn = new QPushButton("▶  Calibrate");
    connect(d->calibrateBtn, &QPushButton::clicked, this, [this] {
        // Board spec is kept live-synced by sync_board_spec() above —
        // nothing to rebuild here.
        d->manager.calibrate();
    });
    row1->addWidget(d->calibrateBtn);

    row1->addSpacing(12);
    row1->addWidget(new QLabel("RMS error:"));
    d->rmsLabel = new QLabel("—");
    d->rmsLabel->setFixedWidth(70);
    row1->addWidget(d->rmsLabel);
    row1->addStretch();
    vlay->addLayout(row1);

    auto* row2     = new QHBoxLayout;
    auto add_param = [&](const QString& name, QLabel*& label) {
        row2->addWidget(new QLabel(name));
        label = new QLabel("—");
        label->setFixedWidth(70);
        label->setProperty("role", "muted");
        row2->addWidget(label);
        row2->addSpacing(8);
    };
    add_param("fx:", d->fxLabel);
    add_param("fy:", d->fyLabel);
    add_param("cx:", d->cxLabel);
    add_param("cy:", d->cyLabel);
    row2->addStretch();
    vlay->addLayout(row2);

    d->saveBtn = new QPushButton("Save calibration to settings");
    d->saveBtn->setEnabled(false);
    connect(d->saveBtn, &QPushButton::clicked, this, &CalibrationW::save_to_settings);
    vlay->addWidget(d->saveBtn);

    parent->addWidget(box);
}

// ── Slots ──────────────────────────────────────────────────────────────────

void CalibrationW::on_corners_detected(int /*viewIndex*/, bool found, QImage preview) {
    const int cnt = d->manager.view_count();
    d->viewCountLabel->setText(QString("Views accepted: %1  (need ≥10)").arg(cnt));

    if (!preview.isNull()) {
        const QPixmap px =
            QPixmap::fromImage(preview).scaledToHeight(160, Qt::SmoothTransformation);
        d->previewLabel->setPixmap(px);
    }

    if (!found) {
        d->previewLabel->setToolTip("No checkerboard corners detected in this frame.");
    } else {
        d->previewLabel->setToolTip(QString("View %1 accepted.").arg(cnt));
    }
}

void CalibrationW::on_calibration_done(double rmsError, bool success) {
    d->calibrateBtn->setEnabled(true);
    d->calibrateBtn->setText("▶  Calibrate");

    if (!success) {
        d->rmsLabel->setText("Failed");
        d->rmsLabel->setStyleSheet(badge_stylesheet(false));
        return;
    }

    update_result_labels();
    d->saveBtn->setEnabled(d->manager.has_result());

    const RmsQuality quality = rms_quality_for(rmsError);
    const QString qualityStr = quality == RmsQuality::Excellent    ? "excellent"
                               : quality == RmsQuality::Good       ? "good"
                               : quality == RmsQuality::Acceptable ? "acceptable"
                                                                   : "poor";
    d->rmsLabel->setToolTip(QString("Reprojection error: %1 px (%2)\n"
                                    "< 0.5 px = excellent, 1–2 px = acceptable, > 2 px = poor")
                                .arg(rmsError, 0, 'f', 3)
                                .arg(qualityStr));
}

void CalibrationW::update_result_labels() {
    if (!d->manager.has_result()) {
        const QString dash = "—";
        d->rmsLabel->setText(dash);
        d->rmsLabel->setStyleSheet(QString());
        d->fxLabel->setText(dash);
        d->fyLabel->setText(dash);
        d->cxLabel->setText(dash);
        d->cyLabel->setText(dash);
        return;
    }
    const auto cal = d->manager.result();
    d->rmsLabel->setText(QString("%1 px").arg(cal.rmsError, 0, 'f', 3));
    d->rmsLabel->setStyleSheet(badge_stylesheet(rms_quality_for(cal.rmsError)));
    d->fxLabel->setText(QString("%1").arg(cal.cameraMatrix[0], 0, 'f', 1));
    d->fyLabel->setText(QString("%1").arg(cal.cameraMatrix[4], 0, 'f', 1));
    d->cxLabel->setText(QString("%1").arg(cal.cameraMatrix[2], 0, 'f', 1));
    d->cyLabel->setText(QString("%1").arg(cal.cameraMatrix[5], 0, 'f', 1));
}

void CalibrationW::save_to_settings() {
    if (!d->manager.has_result()) {
        return;
    }
    const int camIdx = d->cameraCombo->currentIndex();
    if (camIdx < 0 || camIdx >= static_cast<int>(d->videoSettings.cameras.size())) {
        return;
    }

    // Overwrite only the intrinsic fields — a whole-struct assignment would
    // also stomp extrinsicRt/extrinsicCalibrated back to their defaults if
    // the Room (Extrinsics) tab had already solved this camera's pose,
    // silently discarding it every time intrinsics are re-run.
    const CalibrationData intrinsics = d->manager.result();
    CalibrationData& stored = d->videoSettings.cameras[static_cast<size_t>(camIdx)].calibration;
    stored.calibrated       = intrinsics.calibrated;
    stored.rmsError         = intrinsics.rmsError;
    stored.cameraMatrix     = intrinsics.cameraMatrix;
    stored.distCoeffs       = intrinsics.distCoeffs;
    emit calibration_saved(camIdx);

    QMessageBox::information(this, "Calibration saved",
                             QString("Camera %1 calibration stored in settings.\n"
                                     "It will be written to the settings file on application exit.")
                                 .arg(camIdx));
}

} // namespace mosaic
