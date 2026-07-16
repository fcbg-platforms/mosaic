#include "ui/video/performance_monitor_w.hpp"
#include "analysis/pose_models.hpp"
#include <algorithm>
#include <limits>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

namespace mosaic {

// ── Mini horizontal fill bar ───────────────────────────────────────────────

class FillBar : public QWidget {
public:
    explicit FillBar(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedHeight(14);
        setMinimumWidth(60);
    }
    void set_value(int pct) {
        m_pct = std::clamp(pct, 0, 100);
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF bg(0, 2, width(), height() - 4);
        p.setBrush(QColor("#0d0d1e"));
        p.setPen(QColor("#1e1e3a"));
        p.drawRoundedRect(bg, 3, 3);
        if (m_pct > 0) {
            const double fillW = (bg.width() - 2) * m_pct / 100.0;
            const QColor fillCol = m_pct > 80 ? QColor("#cc4444")
                                 : m_pct > 50 ? QColor("#ccaa44")
                                              : QColor("#44aacc");
            p.setBrush(fillCol);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(QRectF(1, 3, fillW, height() - 6), 2, 2);
        }
        // Percentage text
        p.setPen(QColor("#888899"));
        QFont font;
        font.setPixelSize(9);
        p.setFont(font);
        p.drawText(QRectF(0, 0, width(), height()),
                   Qt::AlignCenter, QString("%1%").arg(m_pct));
    }
private:
    int m_pct{0};
};

// ── Camera row ─────────────────────────────────────────────────────────────

struct CameraRow {
    QLabel*   statusDot  = nullptr;
    QLabel*   fpsLbl     = nullptr;
    QLabel*   grabbedLbl = nullptr;
    QLabel*   encodedLbl = nullptr;
    QLabel*   droppedLbl = nullptr;
    FillBar*  ringBar    = nullptr;
};

// ── Impl ───────────────────────────────────────────────────────────────────

struct PerformanceMonitorW::Impl {
    VideoManager*    videoMgr    = nullptr;
    AudioManager*    audioMgr    = nullptr;
    AnalysisManager* analysisMgr = nullptr;

    QTimer*         timer       = nullptr;
    QGridLayout*    grid        = nullptr;
    QWidget*        gridWidget  = nullptr;

    // Summary / global labels
    QLabel* recStatusLbl  = nullptr;
    QLabel* totalEncLbl   = nullptr;
    QLabel* totalDropLbl  = nullptr;
    QLabel* audioLbl      = nullptr;
    QLabel* syncSkewLbl   = nullptr;

    // Analysis section
    QPushButton* analysisToggleBtn = nullptr;
    QComboBox*   modelCombo        = nullptr;
    QTextEdit*   analysisLog       = nullptr;
    QLabel*      analysisStatusLbl = nullptr;
    int          logLineCount      = 0;
    static constexpr int kMaxLogLines = 300;

    std::vector<CameraRow> cameraRows;
    int lastCameraCount = -1;
};

// ── Construction ───────────────────────────────────────────────────────────

PerformanceMonitorW::PerformanceMonitorW(VideoManager*    videoMgr,
                                          AudioManager*    audioMgr,
                                          AnalysisManager* analysisMgr,
                                          QWidget*         parent)
    : QWidget(parent), d(std::make_unique<Impl>())
{
    d->videoMgr    = videoMgr;
    d->audioMgr    = audioMgr;
    d->analysisMgr = analysisMgr;

    build_ui();

    d->timer = new QTimer(this);
    d->timer->setInterval(1000);
    connect(d->timer, &QTimer::timeout, this, &PerformanceMonitorW::on_tick);
    d->timer->start();
}

PerformanceMonitorW::~PerformanceMonitorW() = default;

// ── UI ─────────────────────────────────────────────────────────────────────

void PerformanceMonitorW::build_ui() {
    auto* outerLay = new QVBoxLayout(this);
    outerLay->setContentsMargins(0, 0, 0, 0);
    outerLay->setSpacing(0);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content = new QWidget;
    auto* contentLay = new QVBoxLayout(content);
    contentLay->setContentsMargins(10, 10, 10, 10);
    contentLay->setSpacing(10);

    // ── Camera grid ──────────────────────────────────────────────────────────
    auto* camBox = new QGroupBox("Camera Pipeline");
    auto* camLay = new QVBoxLayout(camBox);
    camLay->setSpacing(4);

    // Column headers
    d->gridWidget = new QWidget;
    d->grid = new QGridLayout(d->gridWidget);
    d->grid->setContentsMargins(0, 0, 0, 0);
    d->grid->setSpacing(4);
    d->grid->setColumnStretch(0, 0);  // status dot
    d->grid->setColumnStretch(1, 0);  // camera label
    d->grid->setColumnStretch(2, 1);  // fps
    d->grid->setColumnStretch(3, 1);  // grabbed
    d->grid->setColumnStretch(4, 1);  // encoded
    d->grid->setColumnStretch(5, 1);  // dropped
    d->grid->setColumnStretch(6, 2);  // ring fill

    const auto headerStyle = "color: #44446a; font-size: 9px; font-weight: bold; letter-spacing: 1px;";
    auto addHeader = [&](int col, const QString& text) {
        auto* lbl = new QLabel(text);
        lbl->setStyleSheet(headerStyle);
        lbl->setAlignment(Qt::AlignCenter);
        d->grid->addWidget(lbl, 0, col);
    };
    addHeader(1, "CAMERA");
    addHeader(2, "FPS");
    addHeader(3, "GRABBED");
    addHeader(4, "ENCODED");
    addHeader(5, "DROPPED");
    addHeader(6, "RING");

    auto* headerLine = new QFrame;
    headerLine->setFrameShape(QFrame::HLine);
    headerLine->setStyleSheet("QFrame { background: #1a1a35; border: none; max-height: 1px; }");
    camLay->addWidget(d->gridWidget);
    camLay->addWidget(headerLine);

    // "No cameras" placeholder — shown when grid has only headers
    auto* noCamera = new QLabel("No cameras configured. Add cameras in the Video tab.");
    noCamera->setStyleSheet("color: #333355; font-size: 11px; padding: 8px;");
    noCamera->setAlignment(Qt::AlignCenter);
    camLay->addWidget(noCamera);

    contentLay->addWidget(camBox);

    // ── Totals box ────────────────────────────────────────────────────────────
    auto* totBox = new QGroupBox("Session Totals");
    auto* totLay = new QGridLayout(totBox);
    totLay->setSpacing(6);

    auto addTotalRow = [&](int row, const QString& label, QLabel*& valueOut) {
        auto* lbl = new QLabel(label);
        lbl->setStyleSheet("color: #7070a0; font-size: 11px;");
        lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        totLay->addWidget(lbl, row, 0);
        valueOut = new QLabel("—");
        valueOut->setStyleSheet("color: #c8c8e0; font-size: 12px; font-family: monospace;");
        totLay->addWidget(valueOut, row, 1);
    };

    addTotalRow(0, "Recording:", d->recStatusLbl);
    addTotalRow(1, "Total encoded:", d->totalEncLbl);
    addTotalRow(2, "Total dropped:", d->totalDropLbl);
    addTotalRow(3, "Audio:", d->audioLbl);
    // Coarse (1 Hz) live cross-camera skew — max minus min of each running
    // camera's most recent elapsed_ns-stamped frame. Distinct from
    // SyncManifest's precise post-hoc per-tick report: this just answers
    // "are the cameras currently roughly in step" during a live session.
    addTotalRow(4, "Sync skew:", d->syncSkewLbl);

    totLay->setColumnStretch(0, 0);
    totLay->setColumnStretch(1, 1);

    contentLay->addWidget(totBox);

    // ── Analysis section ─────────────────────────────────────────────────────
    if (d->analysisMgr) {
        auto* anaBox = new QGroupBox("Pose Estimation  (post-recording)");
        auto* anaLay = new QVBoxLayout(anaBox);
        anaLay->setSpacing(8);

        // Enable toggle + model selector row
        auto* topRow = new QHBoxLayout;

        d->analysisToggleBtn = new QPushButton("Enable auto-analyse");
        d->analysisToggleBtn->setCheckable(true);
        d->analysisToggleBtn->setChecked(false);
        d->analysisToggleBtn->setFixedHeight(28);
        d->analysisToggleBtn->setStyleSheet(
            "QPushButton { background: #0d0d22; border: 1px solid #252545;"
            "  border-radius: 4px; padding: 4px 12px; color: #6666aa; font-size: 11px; }"
            "QPushButton:checked { background: #1a3a1a; border-color: #33aa55; color: #88ffaa; }"
            "QPushButton:hover { background: #14142a; border-color: #4444aa; color: #9999cc; }");
        connect(d->analysisToggleBtn, &QPushButton::toggled, this,
                [this](bool on) {
            d->analysisMgr->set_auto_analyze(on);
            d->analysisToggleBtn->setText(on ? "■ Auto-analyse enabled" : "Enable auto-analyse");
        });
        topRow->addWidget(d->analysisToggleBtn);

        topRow->addSpacing(8);
        auto* modelLbl = new QLabel("Model:");
        modelLbl->setStyleSheet("color: #7070a0; font-size: 11px;");
        topRow->addWidget(modelLbl);

        d->modelCombo = new QComboBox;
        for (const auto& [label, value] : pose_model_options()) {
            d->modelCombo->addItem(label, value);
        }
        d->modelCombo->setCurrentIndex(0);
        d->modelCombo->setFixedHeight(28);
        connect(d->modelCombo, &QComboBox::currentIndexChanged, this,
                [this](int idx) {
            d->analysisMgr->set_model(
                d->modelCombo->itemData(idx).toString());
        });
        topRow->addWidget(d->modelCombo, 1);

        anaLay->addLayout(topRow);

        // Hint text
        auto* hintLbl = new QLabel(
            "When enabled: after each recording stops, MOSAIC automatically "
            "runs YOLOv8 pose estimation on the saved .mp4 files and writes "
            ".pose.json keypoint files alongside them. The status and log "
            "below also reflect any other Analysis Manager job — e.g. a "
            "Face Masking run started from the Analysis tab — since only "
            "one job runs at a time.");
        hintLbl->setWordWrap(true);
        hintLbl->setStyleSheet("color: #444466; font-size: 10px;");
        anaLay->addWidget(hintLbl);

        // Status + log
        d->analysisStatusLbl = new QLabel("Status: idle");
        d->analysisStatusLbl->setStyleSheet("color: #555577; font-size: 10px;");
        anaLay->addWidget(d->analysisStatusLbl);

        d->analysisLog = new QTextEdit;
        d->analysisLog->setReadOnly(true);
        d->analysisLog->setMaximumHeight(140);
        d->analysisLog->setMinimumHeight(60);
        d->analysisLog->setStyleSheet(
            "QTextEdit { background: #060810; color: #44aa66;"
            " font-family: 'Courier New', monospace; font-size: 10px;"
            " border: 1px solid #1a2a1a; border-radius: 4px; padding: 4px; }");
        anaLay->addWidget(d->analysisLog);

        // Wire signals
        connect(d->analysisMgr, &AnalysisManager::analysis_started, this,
                [this](const QString& path) {
            d->analysisStatusLbl->setText(
                QString("Status: <font color='#44cc66'>● Running</font>  →  %1").arg(path));
            d->analysisStatusLbl->setTextFormat(Qt::RichText);
        });
        connect(d->analysisMgr, &AnalysisManager::analysis_finished, this,
                [this](const QString& /*path*/, bool success) {
            d->analysisStatusLbl->setText(
                success ? "Status: <font color='#44aaff'>✓ Finished</font>"
                        : "Status: <font color='#cc4444'>✗ Error</font>");
            d->analysisStatusLbl->setTextFormat(Qt::RichText);
        });
        connect(d->analysisMgr, &AnalysisManager::output_received, this,
                [this](const QString& line) {
            if (d->logLineCount >= Impl::kMaxLogLines) {
                d->analysisLog->clear();
                d->logLineCount = 0;
            }
            d->analysisLog->append(line);
            ++d->logLineCount;
        });
        connect(d->analysisMgr, &AnalysisManager::setup_error, this,
                [this](const QString& msg) {
            d->analysisStatusLbl->setText(
                "<font color='#cc4444'>⚠ Setup error — see log</font>");
            d->analysisStatusLbl->setTextFormat(Qt::RichText);
            d->analysisLog->append("<font color='#cc6644'>" + msg.toHtmlEscaped() + "</font>");
            d->analysisLog->setTextInteractionFlags(Qt::TextSelectableByMouse);
        });

        contentLay->addWidget(anaBox);
    }

    contentLay->addStretch();

    scroll->setWidget(content);
    outerLay->addWidget(scroll);
}

// ── Dynamic camera rows ────────────────────────────────────────────────────

void PerformanceMonitorW::rebuild_camera_rows() {
    const int count = d->videoMgr ? d->videoMgr->camera_count() : 0;
    d->lastCameraCount = count;
    d->cameraRows.clear();

    // Remove existing grid rows (skip header row 0)
    while (d->grid->rowCount() > 1) {
        for (int col = 0; col < d->grid->columnCount(); ++col) {
            auto* item = d->grid->itemAtPosition(d->grid->rowCount() - 1, col);
            if (item) {
                if (item->widget()) { item->widget()->deleteLater(); }
                d->grid->removeItem(item);
                delete item;
            }
        }
    }

    d->cameraRows.resize(static_cast<size_t>(count));

    const auto valStyle = "color: #c8c8e0; font-size: 11px; font-family: monospace;";

    for (int idx = 0; idx < count; ++idx) {
        const int row = idx + 1;
        auto& cr = d->cameraRows[static_cast<size_t>(idx)];

        cr.statusDot = new QLabel("●");
        cr.statusDot->setAlignment(Qt::AlignCenter);
        cr.statusDot->setFixedWidth(16);
        d->grid->addWidget(cr.statusDot, row, 0);

        auto* camLbl = new QLabel(QString("Cam %1").arg(idx + 1));
        camLbl->setStyleSheet("color: #9898cc; font-size: 11px; font-weight: bold;");
        camLbl->setAlignment(Qt::AlignCenter);
        d->grid->addWidget(camLbl, row, 1);

        cr.fpsLbl = new QLabel("—");
        cr.fpsLbl->setStyleSheet(valStyle);
        cr.fpsLbl->setAlignment(Qt::AlignCenter);
        d->grid->addWidget(cr.fpsLbl, row, 2);

        cr.grabbedLbl = new QLabel("—");
        cr.grabbedLbl->setStyleSheet(valStyle);
        cr.grabbedLbl->setAlignment(Qt::AlignCenter);
        d->grid->addWidget(cr.grabbedLbl, row, 3);

        cr.encodedLbl = new QLabel("—");
        cr.encodedLbl->setStyleSheet(valStyle);
        cr.encodedLbl->setAlignment(Qt::AlignCenter);
        d->grid->addWidget(cr.encodedLbl, row, 4);

        cr.droppedLbl = new QLabel("—");
        cr.droppedLbl->setStyleSheet(valStyle);
        cr.droppedLbl->setAlignment(Qt::AlignCenter);
        d->grid->addWidget(cr.droppedLbl, row, 5);

        cr.ringBar = new FillBar;
        d->grid->addWidget(cr.ringBar, row, 6);
    }
}

// ── Tick ──────────────────────────────────────────────────────────────────

void PerformanceMonitorW::on_tick() {
    if (!d->videoMgr) { return; }

    const int count = d->videoMgr->camera_count();

    // Rebuild rows if camera count changed
    if (count != d->lastCameraCount) {
        rebuild_camera_rows();
    }

    // Update per-camera rows, tracking min/max last-frame elapsed_ns across
    // currently-running cameras for the live sync-skew indicator below.
    int64_t minElapsedNs = std::numeric_limits<int64_t>::max();
    int64_t maxElapsedNs = std::numeric_limits<int64_t>::min();
    int     liveCams     = 0;

    for (int idx = 0; idx < count; ++idx) {
        const auto stats = d->videoMgr->camera_stats(idx);
        auto& cr = d->cameraRows[static_cast<size_t>(idx)];

        cr.statusDot->setStyleSheet(
            stats.grabberRunning ? "color: #44cc44;" : "color: #444444;");

        cr.fpsLbl->setText(QString::number(stats.fps, 'f', 1));

        auto fmt = [](int64_t n) {
            return (n > 0) ? QString("%L1").arg(n) : QString("0");
        };
        cr.grabbedLbl->setText(fmt(stats.framesGrabbed));
        cr.encodedLbl->setText(fmt(stats.framesEncoded));

        if (stats.framesDropped > 0) {
            cr.droppedLbl->setText(QString("<font color='#cc4444'>%L1</font>").arg(stats.framesDropped));
            cr.droppedLbl->setTextFormat(Qt::RichText);
        } else {
            cr.droppedLbl->setText("0");
            cr.droppedLbl->setTextFormat(Qt::PlainText);
        }

        cr.ringBar->set_value(stats.ringFillPct);

        if (stats.grabberRunning && stats.lastFrameElapsedNs >= 0) {
            minElapsedNs = std::min(minElapsedNs, stats.lastFrameElapsedNs);
            maxElapsedNs = std::max(maxElapsedNs, stats.lastFrameElapsedNs);
            ++liveCams;
        }
    }

    if (d->syncSkewLbl) {
        if (liveCams >= 2) {
            const double skewMs   = static_cast<double>(maxElapsedNs - minElapsedNs) / 1e6;
            // Rough "1 frame period" reference at a nominal 30 fps tick.
            const QString color   = skewMs <= 33.0  ? "#44cc44"
                                   : skewMs <= 66.0  ? "#ccaa44"
                                                      : "#cc4444";
            d->syncSkewLbl->setText(
                QString("<font color='%1'>%2 ms</font> (%3 cams)")
                    .arg(color).arg(skewMs, 0, 'f', 1).arg(liveCams));
            d->syncSkewLbl->setTextFormat(Qt::RichText);
        } else {
            d->syncSkewLbl->setText("—");
            d->syncSkewLbl->setTextFormat(Qt::PlainText);
        }
    }

    // Update totals
    if (d->recStatusLbl) {
        const bool rec = d->videoMgr->is_recording();
        d->recStatusLbl->setText(rec
            ? "<font color='#ff6666'>● Recording</font>"
            : "<font color='#444466'>■ Idle</font>");
        d->recStatusLbl->setTextFormat(Qt::RichText);
    }
    if (d->totalEncLbl) {
        d->totalEncLbl->setText(
            QString("%L1 frames").arg(d->videoMgr->total_frames_encoded()));
    }
    if (d->totalDropLbl) {
        const int64_t dropped = d->videoMgr->total_frames_dropped();
        if (dropped > 0) {
            d->totalDropLbl->setText(
                QString("<font color='#cc4444'>%L1 frames</font>").arg(dropped));
            d->totalDropLbl->setTextFormat(Qt::RichText);
        } else {
            d->totalDropLbl->setText("0 frames");
            d->totalDropLbl->setTextFormat(Qt::PlainText);
        }
    }
    if (d->audioLbl && d->audioMgr) {
        const int mics = d->audioMgr->recorder_count();
        d->audioLbl->setText(QString("%1 recorder%2")
            .arg(mics).arg(mics != 1 ? "s" : ""));
    }
}

} // namespace mosaic
