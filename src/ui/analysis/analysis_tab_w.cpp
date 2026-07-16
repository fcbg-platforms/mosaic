#include "ui/analysis/analysis_tab_w.hpp"
#include "analysis/pose_analysis_result.hpp"
#include "analysis/pose_kinematics.hpp"
#include "analysis/pose_models.hpp"
#include "session/session_info.hpp"
#include "ui/analysis/pose_overlay_player_w.hpp"
#include <QChart>
#include <QChartView>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLegend>
#include <QLineSeries>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTextEdit>
#include <QTextStream>
#include <QValueAxis>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace mosaic {

// ── MetricsChartW — one keypoint's x/y position over time, with a playhead
//    kept in sync with video playback and click-to-seek ────────────────────
//
// No Q_OBJECT / Qt signals here — following the same convention as this
// codebase's other .cpp-local helper widgets (e.g. SessionRow in
// session_browser_w.cpp), which use a plain callback instead of moc-in-cpp
// machinery for a class that never leaves this file.

class MetricsChartW : public QChartView {
public:
    using SeekCb = std::function<void(int64_t)>;

    explicit MetricsChartW(QWidget* parent = nullptr) : QChartView(parent) {
        auto* chart = new QChart();
        chart->setBackgroundBrush(QColor("#0a0a1a"));
        chart->setBackgroundPen(Qt::NoPen);
        chart->legend()->setVisible(false);
        chart->setMargins(QMargins(6, 6, 6, 6));
        setChart(chart);
        setRenderHint(QPainter::Antialiasing);
        setStyleSheet("background: #0a0a1a; border: none;");

        xSeries_ = new QLineSeries();
        xSeries_->setPen(QPen(QColor("#44aaff"), 2));
        ySeries_ = new QLineSeries();
        ySeries_->setPen(QPen(QColor("#ffaa44"), 2));
        // Single-metric series (Speed/Acceleration) — kept separate from
        // xSeries_/ySeries_ rather than repurposing one of them, so Position
        // mode's two-line plot and single-metric mode's one-line plot never
        // fight over which series is "the x-position line".
        valueSeries_ = new QLineSeries();
        valueSeries_->setPen(QPen(QColor("#44aaff"), 2));
        playheadSeries_ = new QLineSeries();
        playheadSeries_->setPen(QPen(QColor("#ff4466"), 1, Qt::DashLine));

        chart->addSeries(xSeries_);
        chart->addSeries(ySeries_);
        chart->addSeries(valueSeries_);
        chart->addSeries(playheadSeries_);

        axisX_ = new QValueAxis();
        axisX_->setLabelFormat("%d");
        axisX_->setTitleText("ms");
        axisX_->setLabelsColor(QColor("#7070a0"));
        axisX_->setTitleBrush(QColor("#7070a0"));
        axisX_->setGridLineColor(QColor("#181838"));
        axisX_->setRange(0, 1000);

        axisY_ = new QValueAxis();
        axisY_->setTitleText("px");
        axisY_->setLabelsColor(QColor("#7070a0"));
        axisY_->setTitleBrush(QColor("#7070a0"));
        axisY_->setGridLineColor(QColor("#181838"));
        axisY_->setRange(0, 1080);

        chart->addAxis(axisX_, Qt::AlignBottom);
        chart->addAxis(axisY_, Qt::AlignLeft);
        for (auto* series : {xSeries_, ySeries_, valueSeries_, playheadSeries_}) {
            series->attachAxis(axisX_);
            series->attachAxis(axisY_);
        }
    }

    void set_seek_callback(SeekCb cb) { seekCb_ = std::move(cb); }

    // subjectIndex picks which detected subject to plot when a frame has
    // more than one (0 = first/primary subject — the common case).
    void set_data(const PoseAnalysisResult& result, int keypointIndex, int subjectIndex = 0) {
        clear_all_series();
        axisY_->setTitleText("px");

        if (!result.is_valid() || result.frames().isEmpty() || keypointIndex < 0) {
            apply_ranges(false, 0.0, 0.0, 0.0);
            return;
        }

        const int64_t t0 = result.frames().first().timestampNs;
        double minY = std::numeric_limits<double>::max();
        double maxY = std::numeric_limits<double>::lowest();
        double maxT = 0.0;

        for (const auto& frame : result.frames()) {
            if (subjectIndex >= frame.subjects.size()) { continue; }
            const auto& subject = frame.subjects[subjectIndex];
            if (keypointIndex >= subject.keypoints.size()) { continue; }

            const double tMs = (frame.timestampNs - t0) / 1e6;
            const auto&  kp  = subject.keypoints[keypointIndex];
            xSeries_->append(tMs, kp.x());
            ySeries_->append(tMs, kp.y());

            maxT = std::max(maxT, tMs);
            minY = std::min({minY, kp.x(), kp.y()});
            maxY = std::max({maxY, kp.x(), kp.y()});
        }

        apply_ranges(minY <= maxY, minY, maxY, maxT);
    }

    // Single-metric mode (Speed/Acceleration): points are (ms-since-start,
    // value), pre-filtered by the caller to drop NaN entries (a NaN plotted
    // point would otherwise break QLineSeries' range calculation). Clears
    // xSeries_/ySeries_ so Position mode's two-line plot doesn't linger
    // underneath.
    void set_single_series(const QVector<QPointF>& points, const QString& yAxisLabel) {
        clear_all_series();
        axisY_->setTitleText(yAxisLabel);

        if (points.isEmpty()) {
            apply_ranges(false, 0.0, 0.0, 0.0);
            return;
        }

        double minY = std::numeric_limits<double>::max();
        double maxY = std::numeric_limits<double>::lowest();
        double maxT = 0.0;

        for (const auto& p : points) {
            valueSeries_->append(p);
            maxT = std::max(maxT, p.x());
            minY = std::min(minY, p.y());
            maxY = std::max(maxY, p.y());
        }

        apply_ranges(true, minY, maxY, maxT);
    }

    void set_playhead_ms(int64_t ms) {
        playheadSeries_->clear();
        playheadSeries_->append(static_cast<double>(ms), axisY_->min());
        playheadSeries_->append(static_cast<double>(ms), axisY_->max());
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (seekCb_ && axisX_->max() > axisX_->min()) {
            const QPointF scenePos = mapToScene(event->pos());
            const QPointF chartPos = chart()->mapFromScene(scenePos);
            const QPointF value    = chart()->mapToValue(chartPos, xSeries_);
            const int64_t ms = static_cast<int64_t>(
                std::clamp(value.x(), axisX_->min(), axisX_->max()));
            seekCb_(ms);
        }
        QChartView::mousePressEvent(event);
    }

private:
    void clear_all_series() {
        xSeries_->clear();
        ySeries_->clear();
        valueSeries_->clear();
        playheadSeries_->clear();
    }

    // Shared by set_data()/set_single_series() so both modes' axis-range
    // policy (10%-padded, 5px minimum pad) can't drift apart. hasRange ==
    // false resets to a small fixed default instead of leaving whatever
    // range a previously-displayed mode left behind — a stale wide range
    // under a freshly-changed axis label (e.g. switching to Speed for a
    // keypoint with no valid samples) would otherwise show old numbers next
    // to a new, unrelated unit.
    void apply_ranges(bool hasRange, double minY, double maxY, double maxT) {
        if (!hasRange) {
            axisX_->setRange(0, 1000);
            axisY_->setRange(0, 1);
            return;
        }
        axisX_->setRange(0, std::max(maxT, 1.0));
        if (minY <= maxY) {
            const double pad = std::max((maxY - minY) * 0.1, 5.0);
            axisY_->setRange(minY - pad, maxY + pad);
        }
    }

    QLineSeries* xSeries_        = nullptr;
    QLineSeries* ySeries_        = nullptr;
    QLineSeries* valueSeries_    = nullptr;
    QLineSeries* playheadSeries_ = nullptr;
    QValueAxis*  axisX_          = nullptr;
    QValueAxis*  axisY_          = nullptr;
    SeekCb       seekCb_;
};

// ── AnalysisTabW::Impl ──────────────────────────────────────────────────

struct AnalysisTabW::Impl {
    AppSettings&     settings;
    AnalysisManager* analysisMgr;

    QListWidget* sessionList = nullptr;
    QComboBox*   pluginCombo = nullptr;
    QComboBox*   modelCombo  = nullptr;
    QSpinBox*    skipSpin    = nullptr;
    QPushButton* runBtn      = nullptr;
    QLabel*      statusLbl   = nullptr;
    QTextEdit*   logView     = nullptr;

    QComboBox*          cameraCombo  = nullptr;
    QComboBox*          keypointCombo= nullptr;
    PoseOverlayPlayerW*  player      = nullptr;
    MetricsChartW*       chart       = nullptr;

    // Kinematics view controls — reshape how the already-loaded
    // currentResult is displayed, not what gets launched (unlike runBox's
    // model/skip controls), so they live in their own row.
    QComboBox*      metricCombo         = nullptr;  // Position / Speed / Acceleration
    QSpinBox*        smoothingSpin       = nullptr;
    QDoubleSpinBox*  scaleSpin           = nullptr;  // mm/px, 1.0 = raw pixels
    QLabel*          kinematicsStatsLbl  = nullptr;
    QPushButton*     exportKinematicsBtn = nullptr;

    QList<SessionInfo>  sessions;
    QString              currentSessionPath;
    PoseAnalysisResult    currentResult;

    // AnalysisManager is a single shared instance (also used by
    // SessionBrowserW's "Run Pose" button and PerformanceMonitorW's
    // auto-analyze toggle) and only ever runs one process at a time, so
    // every output_received()/setup_error() between an analysis_started()
    // and its matching analysis_finished() belongs to whichever path that
    // analysis_started() carried. Track whether that's this tab's selected
    // session so status text/log don't flip for a run started elsewhere.
    bool jobIsMine = false;

    Impl(AppSettings& s, AnalysisManager* mgr) : settings(s), analysisMgr(mgr) {}

    [[nodiscard]] const SessionInfo* current_session() const {
        for (const auto& s : sessions) {
            if (s.path == currentSessionPath) { return &s; }
        }
        return nullptr;
    }

    // Shared by update_kinematics_chart() and export_kinematics_csv() — both
    // need the same "ms since first frame" origin, and both must guard the
    // same empty-frames edge case identically.
    [[nodiscard]] int64_t first_timestamp_ns() const {
        return currentResult.frames().isEmpty() ? 0 : currentResult.frames().first().timestampNs;
    }
};

// ── Construction ─────────────────────────────────────────────────────────

AnalysisTabW::AnalysisTabW(AppSettings& settings, AnalysisManager* analysisMgr, QWidget* parent)
    : QWidget(parent), d(std::make_unique<Impl>(settings, analysisMgr))
{
    build_ui();
    rebuild_session_list();

    connect(analysisMgr, &AnalysisManager::analysis_started, this, [this](const QString& path) {
        d->jobIsMine = (path == d->currentSessionPath);
        d->runBtn->setEnabled(false);   // AnalysisManager only runs one job at a time either way
        if (!d->jobIsMine) { return; }
        d->statusLbl->setText("Running…");
        d->statusLbl->setStyleSheet("color:#ddaa33; font-size:11px;");
        d->logView->clear();
    });
    connect(analysisMgr, &AnalysisManager::output_received, this, [this](const QString& line) {
        if (!d->jobIsMine) { return; }
        d->logView->append(line);
    });
    connect(analysisMgr, &AnalysisManager::setup_error, this, [this](const QString& msg) {
        // No session/path info on this signal — always surface it rather
        // than risk hiding a failure of the user's own just-clicked Run.
        d->statusLbl->setText("Error: " + msg);
        d->statusLbl->setStyleSheet("color:#cc4444; font-size:11px;");
        d->runBtn->setEnabled(true);
    });
    connect(analysisMgr, &AnalysisManager::analysis_finished, this,
            [this](const QString& path, bool success) {
        d->runBtn->setEnabled(true);
        if (!d->jobIsMine) { return; }
        d->statusLbl->setText(success ? "Done." : "Failed — see log.");
        d->statusLbl->setStyleSheet(success
            ? "color:#44cc66; font-size:11px;" : "color:#cc4444; font-size:11px;");
        if (success && path == d->currentSessionPath) {
            rebuild_session_list();
        }
    });
}

AnalysisTabW::~AnalysisTabW() = default;

// ── UI construction ──────────────────────────────────────────────────────

void AnalysisTabW::build_ui() {
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);

    auto* splitter = new QSplitter(Qt::Horizontal);
    root->addWidget(splitter);

    // ── Left: session list ──────────────────────────────────────────────
    auto* leftPanel = new QWidget;
    auto* leftLay   = new QVBoxLayout(leftPanel);
    leftLay->setContentsMargins(0, 0, 0, 0);
    leftLay->addWidget(new QLabel("<b>Sessions</b>"));
    d->sessionList = new QListWidget;
    leftLay->addWidget(d->sessionList, 1);
    splitter->addWidget(leftPanel);

    connect(d->sessionList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || row >= d->sessions.size()) { return; }
        select_session(d->sessions[row].path);
    });

    // ── Right: run controls + results ───────────────────────────────────
    auto* rightPanel = new QWidget;
    auto* rightLay   = new QVBoxLayout(rightPanel);
    rightLay->setContentsMargins(0, 0, 0, 0);

    auto* runBox = new QGroupBox("Run analysis");
    auto* runLay = new QVBoxLayout(runBox);

    auto* controlsRow = new QHBoxLayout;
    d->pluginCombo = new QComboBox;
    d->pluginCombo->addItem("Pose (YOLOv8)", "pose");
    controlsRow->addWidget(new QLabel("Plugin:"));
    controlsRow->addWidget(d->pluginCombo);

    d->modelCombo = new QComboBox;
    for (const auto& [label, value] : pose_model_options()) {
        d->modelCombo->addItem(label, value);
    }
    controlsRow->addWidget(new QLabel("Model:"));
    controlsRow->addWidget(d->modelCombo, 1);

    d->skipSpin = new QSpinBox;
    d->skipSpin->setRange(1, 30);
    d->skipSpin->setValue(1);
    d->skipSpin->setPrefix("skip ");
    controlsRow->addWidget(d->skipSpin);

    d->runBtn = new QPushButton("▶  Run");
    connect(d->runBtn, &QPushButton::clicked, this, &AnalysisTabW::run_analysis);
    controlsRow->addWidget(d->runBtn);
    runLay->addLayout(controlsRow);

    d->statusLbl = new QLabel("Select a session to begin.");
    d->statusLbl->setStyleSheet("color:#6060a0; font-size:11px;");
    runLay->addWidget(d->statusLbl);

    d->logView = new QTextEdit;
    d->logView->setReadOnly(true);
    d->logView->setMaximumHeight(90);
    d->logView->setStyleSheet(
        "QTextEdit { background:#060810; color:#44aa66; font-family:'Courier New',monospace;"
        " font-size:10px; border:1px solid #1a2a1a; border-radius:4px; padding:4px; }");
    runLay->addWidget(d->logView);

    rightLay->addWidget(runBox);

    // ── Results: camera/keypoint pickers + player + chart ──────────────
    auto* resultsRow = new QHBoxLayout;
    d->cameraCombo = new QComboBox;
    resultsRow->addWidget(new QLabel("Camera:"));
    resultsRow->addWidget(d->cameraCombo);
    connect(d->cameraCombo, &QComboBox::currentIndexChanged, this, &AnalysisTabW::select_camera);

    d->keypointCombo = new QComboBox;
    resultsRow->addWidget(new QLabel("Keypoint:"));
    resultsRow->addWidget(d->keypointCombo, 1);
    connect(d->keypointCombo, &QComboBox::currentIndexChanged, this,
            &AnalysisTabW::update_kinematics_chart);
    rightLay->addLayout(resultsRow);

    // ── Kinematics view controls: reshape how the current keypoint's data
    //    is plotted (Position/Speed/Acceleration), not what gets launched.
    auto* kinematicsRow = new QHBoxLayout;
    d->metricCombo = new QComboBox;
    d->metricCombo->addItem("Position", "position");
    d->metricCombo->addItem("Speed", "speed");
    d->metricCombo->addItem("Acceleration", "accel");
    kinematicsRow->addWidget(new QLabel("Metric:"));
    kinematicsRow->addWidget(d->metricCombo);
    connect(d->metricCombo, &QComboBox::currentIndexChanged, this,
            &AnalysisTabW::update_kinematics_chart);

    d->smoothingSpin = new QSpinBox;
    d->smoothingSpin->setRange(1, 15);
    d->smoothingSpin->setSingleStep(2);
    d->smoothingSpin->setValue(1);
    d->smoothingSpin->setPrefix("smooth ");
    d->smoothingSpin->setToolTip(
        "Centered moving-average window (odd values) applied before deriving "
        "Speed/Acceleration. 1 = no smoothing. Raw acceleration from "
        "frame-to-frame pose jitter is often noisy — increase (e.g. 5) to "
        "reduce that noise. Has no effect on the Position view.");
    kinematicsRow->addWidget(d->smoothingSpin);
    connect(d->smoothingSpin, &QSpinBox::valueChanged, this,
            &AnalysisTabW::update_kinematics_chart);

    d->scaleSpin = new QDoubleSpinBox;
    d->scaleSpin->setRange(0.01, 100.0);
    d->scaleSpin->setDecimals(4);
    d->scaleSpin->setValue(1.0);
    d->scaleSpin->setSuffix(" mm/px");
    d->scaleSpin->setToolTip(
        "Optional manual pixel-to-real-world scale, matching the Motion "
        "plugin's mm_per_px convention (there's no calibration data linked "
        "to Pose output). Leave at 1.0 for pixel units. Only affects the "
        "Speed/Acceleration view and stats — Position always displays raw "
        "pixels, and Export CSV always writes raw pixels regardless of this.");
    kinematicsRow->addWidget(d->scaleSpin);
    connect(d->scaleSpin, &QDoubleSpinBox::valueChanged, this,
            &AnalysisTabW::update_kinematics_chart);

    d->kinematicsStatsLbl = new QLabel;
    d->kinematicsStatsLbl->setStyleSheet("color:#7070a0; font-size:11px;");
    d->kinematicsStatsLbl->setToolTip(
        "Subject identity is not tracked across frames — kinematics assume "
        "subject 0 is a single continuous animal (safe for single-subject "
        "sessions only).");
    kinematicsRow->addWidget(d->kinematicsStatsLbl, 1);

    d->exportKinematicsBtn = new QPushButton("Export CSV");
    d->exportKinematicsBtn->setToolTip(
        "Exports timestamp/position/speed/acceleration for the current "
        "keypoint in raw pixel units, ignoring the Scale spinbox above — "
        "includes the subject-identity and smoothing caveats as a comment "
        "header in the file.");
    connect(d->exportKinematicsBtn, &QPushButton::clicked, this,
            &AnalysisTabW::export_kinematics_csv);
    kinematicsRow->addWidget(d->exportKinematicsBtn);

    rightLay->addLayout(kinematicsRow);

    auto* resultsSplitter = new QSplitter(Qt::Horizontal);
    d->player = new PoseOverlayPlayerW;
    resultsSplitter->addWidget(d->player);

    d->chart = new MetricsChartW;
    resultsSplitter->addWidget(d->chart);
    resultsSplitter->setStretchFactor(0, 1);
    resultsSplitter->setStretchFactor(1, 1);
    rightLay->addWidget(resultsSplitter, 1);

    d->chart->set_seek_callback([this](int64_t ms) { d->player->seek(ms); });
    connect(d->player, &PoseOverlayPlayerW::position_changed, this, [this](int64_t ms) {
        d->chart->set_playhead_ms(ms);
    });

    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
}

// ── Session list ─────────────────────────────────────────────────────────

void AnalysisTabW::rebuild_session_list() {
    const QString selected = d->currentSessionPath;
    d->sessions = SessionInfo::list_all(d->settings.record.directory);

    d->sessionList->blockSignals(true);
    d->sessionList->clear();
    int selectRow = -1;
    for (int i = 0; i < d->sessions.size(); ++i) {
        const auto& s = d->sessions[i];
        d->sessionList->addItem(QString("%1  (%2 cam, %3)")
            .arg(s.name).arg(s.cameraCount).arg(s.format_duration()));
        if (s.path == selected) { selectRow = i; }
    }
    d->sessionList->blockSignals(false);

    if (selectRow >= 0) {
        d->sessionList->setCurrentRow(selectRow);
    } else if (!d->sessions.isEmpty()) {
        d->sessionList->setCurrentRow(0);
    } else {
        // No sessions left (e.g. the previously-selected one was deleted
        // externally) — clear selection state instead of leaving
        // currentSessionPath pointing at a session that no longer exists.
        select_session(QString());
    }
}

void AnalysisTabW::select_session(const QString& path) {
    d->currentSessionPath = path;
    const auto* info = d->current_session();

    // A new session starts back at the default kinematics view rather than
    // carrying over whatever Speed/Acceleration/smoothing/scale selection
    // was last used — those are meant to help compare cameras *within* one
    // session, not persist across unrelated sessions. Blocked so this
    // doesn't trigger 3 redundant chart redraws before select_camera()'s
    // final one below.
    d->metricCombo->blockSignals(true);
    d->metricCombo->setCurrentIndex(0);
    d->metricCombo->blockSignals(false);
    d->smoothingSpin->blockSignals(true);
    d->smoothingSpin->setValue(1);
    d->smoothingSpin->blockSignals(false);
    d->scaleSpin->blockSignals(true);
    d->scaleSpin->setValue(1.0);
    d->scaleSpin->blockSignals(false);

    d->cameraCombo->blockSignals(true);
    d->cameraCombo->clear();
    if (info) {
        for (int i = 0; i < info->videoFiles.size(); ++i) {
            d->cameraCombo->addItem(QString("Camera %1").arg(i), info->videoFiles[i]);
        }
    }
    d->cameraCombo->blockSignals(false);

    d->statusLbl->setText(info ? "Ready." : "Session not found.");
    d->statusLbl->setStyleSheet("color:#6060a0; font-size:11px;");

    select_camera(d->cameraCombo->currentIndex());
}

void AnalysisTabW::select_camera(int index) {
    Q_UNUSED(index);
    reload_current_camera_result();
}

// ── Analysis lifecycle ───────────────────────────────────────────────────

QString AnalysisTabW::pose_json_path_for(const QString& videoRelPath) const {
    QString base = videoRelPath;
    const int dot = base.lastIndexOf('.');
    if (dot >= 0) { base.truncate(dot); }
    return base + ".pose.json";
}

void AnalysisTabW::reload_current_camera_result() {
    const auto* info = d->current_session();
    if (!info || d->cameraCombo->currentIndex() < 0) {
        d->currentResult = PoseAnalysisResult();
        d->player->set_pose_result(d->currentResult);
        d->keypointCombo->clear();
        // Explicit call rather than relying on QComboBox::clear() above
        // reentrantly firing currentIndexChanged(-1) into
        // update_kinematics_chart() — that happens to reset the chart/stats
        // today, but only as an accidental side effect of not being wrapped
        // in blockSignals() the way the populated-combo case below is.
        update_kinematics_chart();
        return;
    }

    const QString videoRel = d->cameraCombo->currentData().toString();
    const QString videoAbs = info->path + "/" + videoRel;
    d->player->set_video(videoAbs);

    const QString poseAbs = info->path + "/" + pose_json_path_for(videoRel);
    d->currentResult = QFileInfo::exists(poseAbs)
        ? PoseAnalysisResult::load(poseAbs) : PoseAnalysisResult();
    d->player->set_pose_result(d->currentResult);

    d->keypointCombo->blockSignals(true);
    d->keypointCombo->clear();
    for (const auto& name : d->currentResult.keypoint_names()) {
        d->keypointCombo->addItem(name);
    }
    d->keypointCombo->blockSignals(false);

    update_kinematics_chart();

    if (!d->currentResult.is_valid()) {
        d->statusLbl->setText("No analysis yet for this camera — click Run.");
        d->statusLbl->setStyleSheet("color:#6060a0; font-size:11px;");
    }
}

// ── Kinematics view ──────────────────────────────────────────────────────

void AnalysisTabW::update_kinematics_chart() {
    const int keypointIndex = d->keypointCombo->currentIndex();
    const QString metric = d->metricCombo->currentData().toString();

    if (metric == "position" || keypointIndex < 0) {
        d->chart->set_data(d->currentResult, keypointIndex);
        d->kinematicsStatsLbl->clear();
        d->chart->set_playhead_ms(d->player->position_ms());
        return;
    }

    const bool isSpeed = metric == "speed";
    const auto series = compute_kinematics(d->currentResult, keypointIndex,
                                            /*subjectIndex=*/0, d->smoothingSpin->value());
    const double scale = d->scaleSpin->value();  // mm/px, 1.0 = raw pixels
    const bool   isMm  = scale != 1.0;
    const QString unit = isSpeed ? (isMm ? "mm/s" : "px/s") : (isMm ? "mm/s²" : "px/s²");

    QVector<QPointF> points;
    points.reserve(series.samples.size());
    const int64_t t0 = d->first_timestamp_ns();
    for (const auto& sample : series.samples) {
        const double value = isSpeed ? sample.speedPxPerS : sample.accelPxPerS2;
        if (std::isnan(value)) { continue; }
        const double tMs = (sample.timestampNs - t0) / 1e6;
        points.append(QPointF(tMs, value * scale));
    }

    d->chart->set_single_series(points, unit);
    d->chart->set_playhead_ms(d->player->position_ms());

    if (std::isnan(series.stats.avgSpeedPxPerS)) {
        d->kinematicsStatsLbl->setText("Not enough data for stats (need ≥2 valid samples).");
    } else {
        d->kinematicsStatsLbl->setText(QString(
            "Distance: %1  ·  Avg speed: %2 %3  ·  Max speed: %4 %3")
            .arg(series.stats.totalDistancePx * scale, 0, 'f', 1)
            .arg(series.stats.avgSpeedPxPerS * scale, 0, 'f', 1)
            .arg(isMm ? "mm/s" : "px/s")
            .arg(series.stats.maxSpeedPxPerS * scale, 0, 'f', 1));
    }
}

void AnalysisTabW::export_kinematics_csv() {
    const auto* info = d->current_session();
    const int keypointIndex = d->keypointCombo->currentIndex();
    if (!info || keypointIndex < 0 || !d->currentResult.is_valid()) { return; }

    const QString keypointName = d->keypointCombo->currentText();
    const QString cameraLabel  = d->cameraCombo->currentText();
    const QString suggested = info->path + "/" + cameraLabel + "_" + keypointName
        + "_kinematics.csv";

    const QString dst = QFileDialog::getSaveFileName(
        this, "Export Kinematics", suggested, "CSV files (*.csv)");
    if (dst.isEmpty()) { return; }

    QFile f(dst);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) { return; }

    // Always raw pixel units, regardless of the display-layer mm/px scale —
    // unambiguous for anyone reading the file without knowing what scale
    // was selected in the UI at export time.
    const int smoothingWindow = d->smoothingSpin->value();
    const auto series = compute_kinematics(d->currentResult, keypointIndex,
                                            /*subjectIndex=*/0, smoothingWindow);
    QTextStream ts(&f);
    // Both caveats below matter to anyone reading this file without having
    // seen the app: subject identity isn't tracked frame-to-frame by the
    // Pose detector (a stat like max speed can be corrupted by an identity
    // swap in a multi-subject session), and x_px/y_px are post-smoothing,
    // not the raw detector output.
    ts << "# subject_index=0 - identity is not tracked across frames by the "
          "Pose detector; treat as one continuous animal only for "
          "single-subject sessions\n";
    ts << "# smoothing_window=" << smoothingWindow
       << " (x_px/y_px below are post-smoothing positions)\n";
    ts << "timestamp_ms,x_px,y_px,speed_px_s,accel_px_s2\n";
    const int64_t t0 = d->first_timestamp_ns();
    for (const auto& sample : series.samples) {
        ts << (sample.timestampNs - t0) / 1e6 << ","
           << sample.position.x() << "," << sample.position.y() << ","
           << (std::isnan(sample.speedPxPerS)  ? QString() : QString::number(sample.speedPxPerS))
           << ","
           << (std::isnan(sample.accelPxPerS2) ? QString() : QString::number(sample.accelPxPerS2))
           << "\n";
    }
}

void AnalysisTabW::run_analysis() {
    if (d->currentSessionPath.isEmpty()) { return; }

    d->analysisMgr->set_model(d->modelCombo->currentData().toString());
    d->analysisMgr->set_frame_skip(d->skipSpin->value());
    d->analysisMgr->analyze_session(d->currentSessionPath);
}

} // namespace mosaic
