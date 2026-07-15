#include "ui/calibration/calibration_w.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

namespace mosaic {

struct CalibrationW::Impl {
    VideoSettings&      videoSettings;
    CalibrationManager  manager;

    // Board configuration
    QSpinBox*       colsSpin      = nullptr;
    QSpinBox*       rowsSpin      = nullptr;
    QDoubleSpinBox* squareSpin    = nullptr;

    // Capture controls
    QComboBox*   cameraCombo      = nullptr;
    QLabel*      viewCountLabel   = nullptr;

    // Preview
    QLabel*      previewLabel     = nullptr;

    // Result
    QLabel*      rmsLabel         = nullptr;
    QLabel*      fxLabel          = nullptr;
    QLabel*      fyLabel          = nullptr;
    QLabel*      cxLabel          = nullptr;
    QLabel*      cyLabel          = nullptr;
    QPushButton* saveBtn          = nullptr;
    QPushButton* calibrateBtn     = nullptr;

    explicit Impl(VideoSettings& vs) : videoSettings(vs) {}
};

CalibrationW::CalibrationW(VideoSettings& videoSettings, QWidget* parent)
    : QWidget(parent), d(std::make_unique<Impl>(videoSettings))
{
    auto* outerLay = new QVBoxLayout(this);
    outerLay->setContentsMargins(0, 0, 0, 0);

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
        outerLay->addWidget(scroll);
        return;
    }

    build_board_section(contentLay);
    build_capture_section(contentLay);
    build_preview_section(contentLay);
    build_result_section(contentLay);
    contentLay->addStretch();

    scroll->setWidget(content);
    outerLay->addWidget(scroll);

    // Connect CalibrationManager signals.
    connect(&d->manager, &CalibrationManager::corners_detected,
            this,         &CalibrationW::on_corners_detected);
    connect(&d->manager, &CalibrationManager::calibration_done,
            this,         &CalibrationW::on_calibration_done);
    connect(&d->manager, &CalibrationManager::calibration_started, this, [this] {
        d->calibrateBtn->setEnabled(false);
        d->calibrateBtn->setText("Calibrating…");
    });
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
    d->squareSpin->setValue(25.0);
    d->squareSpin->setSuffix("  mm");
    d->squareSpin->setFixedWidth(95);
    row->addWidget(d->squareSpin);
    row->addStretch();

    parent->addWidget(box);
}

// ── Capture section ────────────────────────────────────────────────────────

void CalibrationW::build_capture_section(QVBoxLayout* parent) {
    auto* box = new QGroupBox("Capture");
    auto* vlay = new QVBoxLayout(box);

    auto* row1 = new QHBoxLayout;

    row1->addWidget(new QLabel("Camera:"));
    d->cameraCombo = new QComboBox;
    for (int i = 0; i < static_cast<int>(d->videoSettings.cameras.size()); ++i) {
        const QString name = d->videoSettings.cameras[static_cast<size_t>(i)].friendlyName;
        d->cameraCombo->addItem(QString("Camera %1 — %2").arg(i).arg(name));
    }
    if (d->cameraCombo->count() == 0) { d->cameraCombo->addItem("No cameras configured"); }
    row1->addWidget(d->cameraCombo, 1);
    vlay->addLayout(row1);

    auto* row2 = new QHBoxLayout;
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
    auto* box = new QGroupBox("Last frame preview");
    auto* vlay = new QVBoxLayout(box);

    d->previewLabel = new QLabel("(no preview)");
    d->previewLabel->setAlignment(Qt::AlignCenter);
    d->previewLabel->setMinimumHeight(160);
    d->previewLabel->setStyleSheet("QLabel { background: #09090f; border-radius: 4px; "
                                    "color: #404060; font-size: 11px; }");
    vlay->addWidget(d->previewLabel);

    parent->addWidget(box);
}

// ── Result section ─────────────────────────────────────────────────────────

void CalibrationW::build_result_section(QVBoxLayout* parent) {
    auto* box = new QGroupBox("Result");
    auto* vlay = new QVBoxLayout(box);

    auto* row1 = new QHBoxLayout;
    d->calibrateBtn = new QPushButton("▶  Calibrate");
    connect(d->calibrateBtn, &QPushButton::clicked, this, [this] {
        CalibrationManager::BoardSpec spec;
        spec.cols         = d->colsSpin->value();
        spec.rows         = d->rowsSpin->value();
        spec.squareSizeMm = d->squareSpin->value();
        d->manager.set_board(spec);
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

    auto* row2 = new QHBoxLayout;
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
    d->viewCountLabel->setText(
        QString("Views accepted: %1  (need ≥10)").arg(cnt));

    if (!preview.isNull()) {
        const QPixmap px = QPixmap::fromImage(preview).scaledToHeight(
            160, Qt::SmoothTransformation);
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
        d->rmsLabel->setStyleSheet("color: #dd4444;");
        return;
    }

    update_result_labels();
    d->saveBtn->setEnabled(d->manager.has_result());

    const QString quality = rmsError < 0.5 ? "excellent" :
                             rmsError < 1.0 ? "good"      :
                             rmsError < 2.0 ? "acceptable": "poor";
    d->rmsLabel->setToolTip(
        QString("Reprojection error: %1 px (%2)\n"
                "< 0.5 px = excellent, 1–2 px = acceptable, > 2 px = poor")
            .arg(rmsError, 0, 'f', 3).arg(quality));
}

void CalibrationW::update_result_labels() {
    if (!d->manager.has_result()) {
        const QString dash = "—";
        d->rmsLabel->setText(dash);
        d->fxLabel->setText(dash);
        d->fyLabel->setText(dash);
        d->cxLabel->setText(dash);
        d->cyLabel->setText(dash);
        return;
    }
    const auto cal = d->manager.result();
    d->rmsLabel->setText(QString("%1 px").arg(cal.rmsError, 0, 'f', 3));
    d->fxLabel->setText(QString("%1").arg(cal.cameraMatrix[0], 0, 'f', 1));
    d->fyLabel->setText(QString("%1").arg(cal.cameraMatrix[4], 0, 'f', 1));
    d->cxLabel->setText(QString("%1").arg(cal.cameraMatrix[2], 0, 'f', 1));
    d->cyLabel->setText(QString("%1").arg(cal.cameraMatrix[5], 0, 'f', 1));
}

void CalibrationW::save_to_settings() {
    if (!d->manager.has_result()) { return; }
    const int camIdx = d->cameraCombo->currentIndex();
    if (camIdx < 0 || camIdx >= static_cast<int>(d->videoSettings.cameras.size())) { return; }

    d->videoSettings.cameras[static_cast<size_t>(camIdx)].calibration = d->manager.result();
    emit calibration_saved(camIdx);

    QMessageBox::information(this, "Calibration saved",
        QString("Camera %1 calibration stored in settings.\n"
                "It will be written to the settings file on application exit.")
            .arg(camIdx));
}

} // namespace mosaic
