#include "ui/analysis/analysis_tab_w.hpp"
#include "analysis/pose_analysis_result.hpp"
#include "analysis/pose_kinematics.hpp"
#include "analysis/pose_models.hpp"
#include "analysis/transcript_result.hpp"
#include "session/session_info.hpp"
#include "ui/analysis/pose_overlay_player_w.hpp"
#include <QAbstractItemView>
#include <QCheckBox>
#include <QChart>
#include <QChartView>
#include <QComboBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLegend>
#include <QLineEdit>
#include <QLineSeries>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTextStream>
#include <QUrl>
#include <QValueAxis>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace mosaic {

namespace {

// Shared by export_kinematics_csv()/export_transcript_csv() (and, in spirit,
// SessionBrowserW::export_annot_csv()) — the QFileDialog/QFile/QTextStream
// save-as skeleton is otherwise duplicated verbatim at every CSV export site
// in the app. Returns false (no file written) if the user cancels the
// dialog or the chosen path can't be opened for writing.
bool export_csv(QWidget* parent, const QString& dialogTitle, const QString& suggestedPath,
                 const std::function<void(QTextStream&)>& writeBody) {
    const QString dst = QFileDialog::getSaveFileName(
        parent, dialogTitle, suggestedPath, "CSV files (*.csv)");
    if (dst.isEmpty()) { return false; }

    QFile f(dst);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) { return false; }

    QTextStream ts(&f);
    writeBody(ts);
    return true;
}

} // namespace

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

    // Plugin-specific run controls, swapped via controlsStack on plugin change.
    QStackedWidget* controlsStack = nullptr;
    QComboBox*   modelCombo   = nullptr;   // pose
    QSpinBox*    skipSpin     = nullptr;   // pose
    QComboBox*   backendCombo = nullptr;   // face_mask
    QComboBox*   styleCombo   = nullptr;   // face_mask
    QSpinBox*    faceSkipSpin = nullptr;   // face_mask
    QComboBox*   whisperModelCombo    = nullptr;   // diarize
    QComboBox*   languageCombo        = nullptr;   // diarize
    QLineEdit*   hfTokenEdit          = nullptr;   // diarize
    QSpinBox*    minSpeakersSpin      = nullptr;   // diarize
    QSpinBox*    maxSpeakersSpin      = nullptr;   // diarize
    QCheckBox*   skipDiarizationCheck = nullptr;   // diarize

    QPushButton* runBtn      = nullptr;
    QLabel*      statusLbl   = nullptr;
    QTextEdit*   logView     = nullptr;

    // Source-picker rows: sourceRowW (Camera/Keypoint, pose+face_mask) and
    // micRowW (Mic, diarize) are mutually exclusive — select_plugin() shows
    // exactly one, mirroring how kinematicsRowW is toggled.
    QWidget*             sourceRowW    = nullptr;
    QComboBox*          cameraCombo   = nullptr;   // pose + face_mask
    QComboBox*          keypointCombo = nullptr;   // pose only
    PoseOverlayPlayerW*  player       = nullptr;   // shared: video (pose/face_mask) or audio (diarize)
    MetricsChartW*       chart        = nullptr;   // pose only
    QPushButton*         openFolderBtn= nullptr;   // face_mask only

    QWidget*     micRowW            = nullptr;   // diarize only
    QComboBox*   micCombo           = nullptr;   // diarize only
    QLabel*      transcriptStatsLbl = nullptr;   // diarize only
    QPushButton* exportTranscriptBtn= nullptr;   // diarize only
    QTableWidget* transcriptTable   = nullptr;   // diarize only

    // Kinematics view controls — reshape how the already-loaded
    // currentResult is displayed, not what gets launched (unlike runBox's
    // model/skip controls), so they live in their own row. Pose only —
    // kinematicsRowW lets select_plugin() hide the whole row with one call.
    QWidget*         kinematicsRowW      = nullptr;
    QComboBox*      metricCombo         = nullptr;  // Position / Speed / Acceleration
    QSpinBox*        smoothingSpin       = nullptr;
    QDoubleSpinBox*  scaleSpin           = nullptr;  // mm/px, 1.0 = raw pixels
    QLabel*          kinematicsStatsLbl  = nullptr;
    QPushButton*     exportKinematicsBtn = nullptr;

    QList<SessionInfo>  sessions;
    QString              currentSessionPath;
    PoseAnalysisResult    currentResult;       // pose only
    TranscriptResult      currentTranscript;   // diarize only

    // AnalysisManager is a single shared instance (also used by
    // SessionBrowserW's "Run Pose" button and PerformanceMonitorW's
    // auto-analyze toggle) and only ever runs one process at a time, so
    // every output_received()/setup_error() between an analysis_started()
    // and its matching analysis_finished() belongs to whichever path that
    // analysis_started() carried. Track whether that's this tab's selected
    // session so status text/log don't flip for a run started elsewhere.
    bool jobIsMine = false;

    // Which plugin run_analysis() launched, captured at launch time. If the
    // user flips pluginCombo to something else before the job finishes,
    // analysis_finished() skips the "Done."/"Failed" status text — it would
    // otherwise be shown against the now-selected (different) plugin's view.
    QString jobPlugin;

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
        // Only touch the status text if the user hasn't switched plugins
        // since starting this job — otherwise "Done."/"Failed" would land
        // on the now-different plugin's view. rebuild_session_list() still
        // runs regardless, so switching back later picks up the result.
        if (d->pluginCombo->currentData().toString() == d->jobPlugin) {
            d->statusLbl->setText(success ? "Done." : "Failed — see log.");
            d->statusLbl->setStyleSheet(success
                ? "color:#44cc66; font-size:11px;" : "color:#cc4444; font-size:11px;");
        }
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
    d->pluginCombo->addItem("Face Masking (anonymize)", "face_mask");
    d->pluginCombo->addItem("Speaker Diarization", "diarize");
    controlsRow->addWidget(new QLabel("Plugin:"));
    controlsRow->addWidget(d->pluginCombo);
    connect(d->pluginCombo, &QComboBox::currentIndexChanged, this, &AnalysisTabW::select_plugin);

    d->controlsStack = new QStackedWidget;

    // ── Pose controls page ──────────────────────────────────────────────
    auto* posePage = new QWidget;
    auto* poseLay  = new QHBoxLayout(posePage);
    poseLay->setContentsMargins(0, 0, 0, 0);

    d->modelCombo = new QComboBox;
    for (const auto& [label, value] : pose_model_options()) {
        d->modelCombo->addItem(label, value);
    }
    poseLay->addWidget(new QLabel("Model:"));
    poseLay->addWidget(d->modelCombo, 1);

    d->skipSpin = new QSpinBox;
    d->skipSpin->setRange(1, 30);
    d->skipSpin->setValue(1);
    d->skipSpin->setPrefix("skip ");
    poseLay->addWidget(d->skipSpin);
    d->controlsStack->addWidget(posePage);

    // ── Face-mask controls page ─────────────────────────────────────────
    auto* faceMaskPage = new QWidget;
    auto* faceMaskLay  = new QHBoxLayout(faceMaskPage);
    faceMaskLay->setContentsMargins(0, 0, 0, 0);

    d->backendCombo = new QComboBox;
    d->backendCombo->addItem("MediaPipe", "mediapipe");
    d->backendCombo->setItemData(0,
        "Best recall, multiple faces. Downloads its model on first use.", Qt::ToolTipRole);
    d->backendCombo->addItem("YOLOv8-face", "yolov8");
    d->backendCombo->setItemData(1,
        "Community-maintained checkpoint — not officially hosted by Ultralytics.",
        Qt::ToolTipRole);
    d->backendCombo->addItem("OpenCV DNN", "opencv");
    d->backendCombo->setItemData(2,
        "No extra ML framework, but weaker recall on extreme head angles.", Qt::ToolTipRole);
    faceMaskLay->addWidget(new QLabel("Backend:"));
    faceMaskLay->addWidget(d->backendCombo, 1);

    d->styleCombo = new QComboBox;
    d->styleCombo->addItem("Blur", "blur");
    d->styleCombo->addItem("Solid box", "box");
    faceMaskLay->addWidget(new QLabel("Style:"));
    faceMaskLay->addWidget(d->styleCombo);

    d->faceSkipSpin = new QSpinBox;
    d->faceSkipSpin->setRange(1, 10);
    d->faceSkipSpin->setValue(1);
    d->faceSkipSpin->setPrefix("skip ");
    d->faceSkipSpin->setToolTip(
        "Detects faces every Nth frame and reuses the last detected boxes in between. "
        "Keep at 1 unless you accept the risk of fast head motion going unmasked on the "
        "skipped frames in between.");
    faceMaskLay->addWidget(d->faceSkipSpin);
    d->controlsStack->addWidget(faceMaskPage);

    // ── Diarization controls page ───────────────────────────────────────
    auto* diarizePage = new QWidget;
    auto* diarizeLay  = new QHBoxLayout(diarizePage);
    diarizeLay->setContentsMargins(0, 0, 0, 0);

    d->whisperModelCombo = new QComboBox;
    d->whisperModelCombo->addItem("tiny",     "tiny");
    d->whisperModelCombo->addItem("base",     "base");
    d->whisperModelCombo->addItem("small",    "small");
    d->whisperModelCombo->addItem("medium",   "medium");
    d->whisperModelCombo->addItem("large-v3", "large-v3");
    d->whisperModelCombo->setCurrentIndex(2);   // small — good speed/accuracy default
    d->whisperModelCombo->setToolTip(
        "faster-whisper model size. Larger models are more accurate but slower "
        "and download more on first use.");
    diarizeLay->addWidget(new QLabel("Model:"));
    diarizeLay->addWidget(d->whisperModelCombo);

    d->languageCombo = new QComboBox;
    d->languageCombo->addItem("Auto-detect", "");
    d->languageCombo->addItem("English",     "en");
    d->languageCombo->addItem("French",      "fr");
    d->languageCombo->addItem("German",      "de");
    d->languageCombo->addItem("Spanish",     "es");
    d->languageCombo->addItem("Italian",     "it");
    d->languageCombo->addItem("Portuguese",  "pt");
    d->languageCombo->addItem("Dutch",       "nl");
    d->languageCombo->addItem("Chinese",     "zh");
    d->languageCombo->addItem("Japanese",    "ja");
    diarizeLay->addWidget(new QLabel("Language:"));
    diarizeLay->addWidget(d->languageCombo);

    d->hfTokenEdit = new QLineEdit;
    d->hfTokenEdit->setEchoMode(QLineEdit::Password);
    d->hfTokenEdit->setPlaceholderText("Hugging Face token (optional — enables speaker labels)");
    d->hfTokenEdit->setText(d->settings.analysis.hfToken);
    d->hfTokenEdit->setToolTip(
        "Required only for speaker diarization (transcription always works without "
        "it). Create a free account at huggingface.co, accept the terms of use for "
        "pyannote/speaker-diarization-3.1 and pyannote/segmentation-3.0, then "
        "generate a token at huggingface.co/settings/tokens. Saved to this profile's "
        "settings so you don't need to re-enter it next time.");
    connect(d->hfTokenEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        d->settings.analysis.hfToken = text;
    });
    diarizeLay->addWidget(d->hfTokenEdit, 1);

    d->minSpeakersSpin = new QSpinBox;
    d->minSpeakersSpin->setRange(0, 20);
    d->minSpeakersSpin->setPrefix("min ");
    d->minSpeakersSpin->setSpecialValueText("min auto");
    d->minSpeakersSpin->setToolTip("Optional hint for pyannote's speaker count (0 = no hint).");
    diarizeLay->addWidget(d->minSpeakersSpin);

    d->maxSpeakersSpin = new QSpinBox;
    d->maxSpeakersSpin->setRange(0, 20);
    d->maxSpeakersSpin->setPrefix("max ");
    d->maxSpeakersSpin->setSpecialValueText("max auto");
    d->maxSpeakersSpin->setToolTip("Optional hint for pyannote's speaker count (0 = no hint).");
    diarizeLay->addWidget(d->maxSpeakersSpin);

    d->skipDiarizationCheck = new QCheckBox("Transcript only");
    d->skipDiarizationCheck->setToolTip(
        "Skip speaker diarization even if a token is set above — faster, and no "
        "network/model-download requirement.");
    diarizeLay->addWidget(d->skipDiarizationCheck);

    d->controlsStack->addWidget(diarizePage);

    controlsRow->addWidget(d->controlsStack, 1);

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
    // sourceRowW/micRowW are mutually exclusive containers (see
    // Impl::sourceRowW doc comment) so select_plugin() can show exactly one.
    d->sourceRowW = new QWidget;
    auto* resultsRow = new QHBoxLayout(d->sourceRowW);
    resultsRow->setContentsMargins(0, 0, 0, 0);
    d->cameraCombo = new QComboBox;
    resultsRow->addWidget(new QLabel("Camera:"));
    resultsRow->addWidget(d->cameraCombo);
    connect(d->cameraCombo, &QComboBox::currentIndexChanged, this, &AnalysisTabW::select_camera);

    d->keypointCombo = new QComboBox;
    resultsRow->addWidget(new QLabel("Keypoint:"));
    resultsRow->addWidget(d->keypointCombo, 1);
    connect(d->keypointCombo, &QComboBox::currentIndexChanged, this,
            &AnalysisTabW::update_kinematics_chart);

    d->openFolderBtn = new QPushButton("Open output folder");
    d->openFolderBtn->setVisible(false);
    d->openFolderBtn->setEnabled(false);
    connect(d->openFolderBtn, &QPushButton::clicked, this, &AnalysisTabW::open_output_folder);
    resultsRow->addWidget(d->openFolderBtn);

    rightLay->addWidget(d->sourceRowW);

    d->micRowW = new QWidget;
    auto* micRow = new QHBoxLayout(d->micRowW);
    micRow->setContentsMargins(0, 0, 0, 0);
    d->micCombo = new QComboBox;
    micRow->addWidget(new QLabel("Mic:"));
    micRow->addWidget(d->micCombo);
    connect(d->micCombo, &QComboBox::currentIndexChanged, this, &AnalysisTabW::select_camera);

    d->transcriptStatsLbl = new QLabel;
    d->transcriptStatsLbl->setStyleSheet("color:#7070a0; font-size:11px;");
    micRow->addWidget(d->transcriptStatsLbl, 1);

    d->exportTranscriptBtn = new QPushButton("Export CSV");
    d->exportTranscriptBtn->setToolTip(
        "Exports timestamp/speaker/text for every segment as CSV.");
    connect(d->exportTranscriptBtn, &QPushButton::clicked, this,
            &AnalysisTabW::export_transcript_csv);
    micRow->addWidget(d->exportTranscriptBtn);

    d->micRowW->setVisible(false);   // shown only for the diarize plugin
    rightLay->addWidget(d->micRowW);

    // ── Kinematics view controls: reshape how the current keypoint's data
    //    is plotted (Position/Speed/Acceleration), not what gets launched.
    //    Pose only — wrapped in a container widget so select_plugin() can
    //    hide the whole row for the Face Masking plugin with one call.
    d->kinematicsRowW = new QWidget;
    auto* kinematicsRow = new QHBoxLayout(d->kinematicsRowW);
    kinematicsRow->setContentsMargins(0, 0, 0, 0);
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

    rightLay->addWidget(d->kinematicsRowW);

    auto* resultsSplitter = new QSplitter(Qt::Horizontal);
    d->player = new PoseOverlayPlayerW;   // reused for audio-only playback in diarize mode too —
    resultsSplitter->addWidget(d->player); // set_video() just calls QMediaPlayer::setSource(),
                                            // which plays a .wav fine with no video frames arriving.

    d->chart = new MetricsChartW;
    resultsSplitter->addWidget(d->chart);

    d->transcriptTable = new QTableWidget(0, 4);
    d->transcriptTable->setHorizontalHeaderLabels({"Start", "End", "Speaker", "Text"});
    d->transcriptTable->horizontalHeader()->setStretchLastSection(true);
    d->transcriptTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    d->transcriptTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    d->transcriptTable->setSelectionMode(QAbstractItemView::SingleSelection);
    d->transcriptTable->setVisible(false);   // shown only for the diarize plugin
    connect(d->transcriptTable, &QTableWidget::cellClicked, this, [this](int row, int) {
        auto* item = d->transcriptTable->item(row, 0);
        if (item) { d->player->seek(item->data(Qt::UserRole).toLongLong()); }
    });
    resultsSplitter->addWidget(d->transcriptTable);

    resultsSplitter->setStretchFactor(0, 1);
    resultsSplitter->setStretchFactor(1, 1);
    resultsSplitter->setStretchFactor(2, 1);
    rightLay->addWidget(resultsSplitter, 1);

    d->chart->set_seek_callback([this](int64_t ms) { d->player->seek(ms); });
    connect(d->player, &PoseOverlayPlayerW::position_changed, this, [this](int64_t ms) {
        d->chart->set_playhead_ms(ms);
        highlight_active_transcript_row(ms);
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

    d->micCombo->blockSignals(true);
    d->micCombo->clear();
    if (info) {
        for (int i = 0; i < info->audioFiles.size(); ++i) {
            d->micCombo->addItem(QString("Mic %1").arg(i), info->audioFiles[i]);
        }
    }
    d->micCombo->blockSignals(false);

    d->statusLbl->setText(info ? "Ready." : "Session not found.");
    d->statusLbl->setStyleSheet("color:#6060a0; font-size:11px;");

    select_camera(d->cameraCombo->currentIndex());
}

void AnalysisTabW::select_camera(int index) {
    Q_UNUSED(index);
    reload_current_camera_result();
}

void AnalysisTabW::select_plugin(int index) {
    if (index < 0) { return; }
    d->controlsStack->setCurrentIndex(index);

    const bool isPose    = is_pose_plugin();
    const bool isDiarize = is_diarize_plugin();
    d->keypointCombo->setVisible(isPose);
    d->chart->setVisible(isPose);
    d->kinematicsRowW->setVisible(isPose);
    d->openFolderBtn->setVisible(!isPose && !isDiarize);
    d->sourceRowW->setVisible(!isDiarize);
    d->micRowW->setVisible(isDiarize);
    d->transcriptTable->setVisible(isDiarize);

    reload_current_camera_result();
}

// ── Analysis lifecycle ───────────────────────────────────────────────────

QString AnalysisTabW::pose_json_path_for(const QString& videoRelPath) const {
    QString base = videoRelPath;
    const int dot = base.lastIndexOf('.');
    if (dot >= 0) { base.truncate(dot); }
    return base + ".pose.json";
}

QString AnalysisTabW::anonymized_video_path_for(const QString& videoRelPath) const {
    return "anonymized/" + QFileInfo(videoRelPath).fileName();
}

QString AnalysisTabW::transcript_json_path_for(const QString& audioRelPath) const {
    QString base = audioRelPath;
    const int dot = base.lastIndexOf('.');
    if (dot >= 0) { base.truncate(dot); }
    return base + ".transcript.json";
}

bool AnalysisTabW::is_pose_plugin() const {
    return d->pluginCombo->currentData().toString() == "pose";
}

bool AnalysisTabW::is_diarize_plugin() const {
    return d->pluginCombo->currentData().toString() == "diarize";
}

void AnalysisTabW::reload_current_camera_result() {
    const auto* info = d->current_session();

    if (is_diarize_plugin()) {
        d->currentResult     = PoseAnalysisResult();
        d->currentTranscript = TranscriptResult();

        if (!info || d->micCombo->currentIndex() < 0) {
            d->player->set_video(QString());   // stop/clear any previously-loaded audio
            d->player->set_pose_result(d->currentResult);
            update_transcript_table();
            return;
        }

        const QString audioRel = d->micCombo->currentData().toString();
        const QString audioAbs = info->path + "/" + audioRel;
        d->player->set_video(audioAbs);
        d->player->set_pose_result(d->currentResult);

        const QString transcriptAbs = info->path + "/" + transcript_json_path_for(audioRel);
        d->currentTranscript = QFileInfo::exists(transcriptAbs)
            ? TranscriptResult::load(transcriptAbs) : TranscriptResult();
        update_transcript_table();

        if (!d->currentTranscript.is_valid()) {
            d->statusLbl->setText("No transcript yet for this mic — click Run.");
            d->statusLbl->setStyleSheet("color:#6060a0; font-size:11px;");
        } else if (!d->currentTranscript.has_diarization()) {
            d->statusLbl->setText(
                "Transcript loaded (no speaker labels — diarization was skipped or unavailable).");
            d->statusLbl->setStyleSheet("color:#6060a0; font-size:11px;");
        }
        return;
    }

    if (!info || d->cameraCombo->currentIndex() < 0) {
        d->currentResult = PoseAnalysisResult();
        d->player->set_video(QString());   // stop/clear any previously-loaded video
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

    if (is_pose_plugin()) {
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

        // Only overwrite statusLbl for the "nothing to show yet" case — a
        // valid result leaves whatever's already there alone, so a "Done."
        // set by the analysis_finished handler right before this runs (via
        // rebuild_session_list()) isn't immediately clobbered back to a
        // generic "Ready." in the same call stack.
        if (!d->currentResult.is_valid()) {
            d->statusLbl->setText("No analysis yet for this camera — click Run.");
            d->statusLbl->setStyleSheet("color:#6060a0; font-size:11px;");
        }
        return;
    }

    // Face-mask plugin: no keypoint/chart data, just plain video playback —
    // the anonymized output if it exists yet, otherwise the original as a
    // preview so the user can confirm they picked the right camera/session.
    d->currentResult = PoseAnalysisResult();
    const QString anonAbs = info->path + "/" + anonymized_video_path_for(videoRel);
    const bool hasOutput = QFileInfo::exists(anonAbs);
    d->player->set_video(hasOutput ? anonAbs : videoAbs);
    d->player->set_pose_result(d->currentResult);
    d->openFolderBtn->setEnabled(hasOutput);

    if (hasOutput) {
        d->statusLbl->setText("Showing anonymized output.");
        d->statusLbl->setStyleSheet("color:#44cc66; font-size:11px;");
    } else {
        d->statusLbl->setText("Not yet anonymized (showing original) — click Run.");
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

    // Always raw pixel units, regardless of the display-layer mm/px scale —
    // unambiguous for anyone reading the file without knowing what scale
    // was selected in the UI at export time.
    const int smoothingWindow = d->smoothingSpin->value();
    const auto series = compute_kinematics(d->currentResult, keypointIndex,
                                            /*subjectIndex=*/0, smoothingWindow);
    const int64_t t0 = d->first_timestamp_ns();

    export_csv(this, "Export Kinematics", suggested, [&](QTextStream& ts) {
        // Both caveats below matter to anyone reading this file without
        // having seen the app: subject identity isn't tracked frame-to-frame
        // by the Pose detector (a stat like max speed can be corrupted by an
        // identity swap in a multi-subject session), and x_px/y_px are
        // post-smoothing, not the raw detector output.
        ts << "# subject_index=0 - identity is not tracked across frames by the "
              "Pose detector; treat as one continuous animal only for "
              "single-subject sessions\n";
        ts << "# smoothing_window=" << smoothingWindow
           << " (x_px/y_px below are post-smoothing positions)\n";
        ts << "timestamp_ms,x_px,y_px,speed_px_s,accel_px_s2\n";
        for (const auto& sample : series.samples) {
            ts << (sample.timestampNs - t0) / 1e6 << ","
               << sample.position.x() << "," << sample.position.y() << ","
               << (std::isnan(sample.speedPxPerS)
                       ? QString() : QString::number(sample.speedPxPerS))
               << ","
               << (std::isnan(sample.accelPxPerS2)
                       ? QString() : QString::number(sample.accelPxPerS2))
               << "\n";
        }
    });
}

// ── Diarization view ─────────────────────────────────────────────────────

void AnalysisTabW::update_transcript_table() {
    const auto& segments = d->currentTranscript.segments();

    d->transcriptTable->setRowCount(segments.size());
    for (int i = 0; i < segments.size(); ++i) {
        const auto& seg = segments[i];

        auto* startItem = new QTableWidgetItem(QString::number(seg.startMs / 1000.0, 'f', 1));
        startItem->setData(Qt::UserRole, static_cast<qlonglong>(seg.startMs));
        startItem->setFlags(startItem->flags() & ~Qt::ItemIsEditable);
        d->transcriptTable->setItem(i, 0, startItem);

        auto* endItem = new QTableWidgetItem(QString::number(seg.endMs / 1000.0, 'f', 1));
        endItem->setFlags(endItem->flags() & ~Qt::ItemIsEditable);
        d->transcriptTable->setItem(i, 1, endItem);

        auto* speakerItem = new QTableWidgetItem(seg.speaker.isEmpty() ? QString("—") : seg.speaker);
        speakerItem->setFlags(speakerItem->flags() & ~Qt::ItemIsEditable);
        d->transcriptTable->setItem(i, 2, speakerItem);

        auto* textItem = new QTableWidgetItem(seg.text);
        textItem->setFlags(textItem->flags() & ~Qt::ItemIsEditable);
        d->transcriptTable->setItem(i, 3, textItem);
    }

    if (!d->currentTranscript.is_valid()) {
        d->transcriptStatsLbl->clear();
        return;
    }

    QStringList speakers;
    for (const auto& seg : segments) {
        if (!seg.speaker.isEmpty() && !speakers.contains(seg.speaker)) {
            speakers << seg.speaker;
        }
    }
    const QString suffix = d->currentTranscript.has_diarization()
        ? QString("%1 speaker(s) detected").arg(speakers.size())
        : QString("diarization skipped — transcript only");
    d->transcriptStatsLbl->setText(QString("%1 segment(s)  ·  %2").arg(segments.size()).arg(suffix));
}

void AnalysisTabW::highlight_active_transcript_row(int64_t ms) {
    if (!is_diarize_plugin()) { return; }

    const auto& segments = d->currentTranscript.segments();
    const auto* seg = d->currentTranscript.segment_at(ms);
    if (!seg) { return; }

    // segment_at() returns a pointer into this exact vector, and
    // update_transcript_table() populates rows 1:1 in the same order, so the
    // row index is just the pointer's offset — no need to scan the table.
    const int row = static_cast<int>(seg - segments.constData());
    if (row < 0 || row >= d->transcriptTable->rowCount()) { return; }

    if (d->transcriptTable->currentRow() != row) {
        d->transcriptTable->selectRow(row);
        if (auto* item = d->transcriptTable->item(row, 0)) {
            d->transcriptTable->scrollToItem(item, QAbstractItemView::PositionAtCenter);
        }
    }
}

void AnalysisTabW::export_transcript_csv() {
    const auto* info = d->current_session();
    if (!info || !d->currentTranscript.is_valid()) { return; }

    const QString micLabel = d->micCombo->currentText();
    const QString suggested = info->path + "/" + micLabel + "_transcript.csv";

    export_csv(this, "Export Transcript", suggested, [&](QTextStream& ts) {
        if (!d->currentTranscript.has_diarization()) {
            ts << "# diarization=skipped - the speaker column is blank for every row\n";
        }
        ts << "start_s,end_s,speaker,text\n";
        for (const auto& seg : d->currentTranscript.segments()) {
            QString text = seg.text;
            text.replace('"', "\"\"");   // minimal CSV escaping — text may contain commas
            ts << (seg.startMs / 1000.0) << "," << (seg.endMs / 1000.0) << ","
               << seg.speaker << ",\"" << text << "\"\n";
        }
    });
}

void AnalysisTabW::run_analysis() {
    if (d->currentSessionPath.isEmpty()) { return; }

    const QString plugin = d->pluginCombo->currentData().toString();

    if (plugin == "diarize") {
        const int minSpeakers = d->minSpeakersSpin->value();
        const int maxSpeakers = d->maxSpeakersSpin->value();
        if (minSpeakers > 0 && maxSpeakers > 0 && minSpeakers > maxSpeakers) {
            d->statusLbl->setText("Error: Min speakers can't exceed Max speakers.");
            d->statusLbl->setStyleSheet("color:#cc4444; font-size:11px;");
            return;
        }
    }

    d->jobPlugin = plugin;
    if (plugin == "pose") {
        d->analysisMgr->set_model(d->modelCombo->currentData().toString());
        d->analysisMgr->set_frame_skip(d->skipSpin->value());
        d->analysisMgr->analyze_session(d->currentSessionPath);
    } else if (plugin == "diarize") {
        d->analysisMgr->run_diarization(d->currentSessionPath,
            d->whisperModelCombo->currentData().toString(),
            d->languageCombo->currentData().toString(),
            d->settings.analysis.hfToken,
            d->minSpeakersSpin->value(),
            d->maxSpeakersSpin->value(),
            d->skipDiarizationCheck->isChecked());
    } else if (plugin == "face_mask") {
        d->analysisMgr->run_face_mask(d->currentSessionPath,
            d->backendCombo->currentData().toString(),
            d->styleCombo->currentData().toString(),
            d->faceSkipSpin->value());
    } else {
        // Defensive: pluginCombo only ever offers the three ids above, but a
        // silent fallthrough here would otherwise launch face-masking with
        // whatever plugin's controls happen to be on screen.
        d->statusLbl->setText("Error: unknown plugin selected.");
        d->statusLbl->setStyleSheet("color:#cc4444; font-size:11px;");
    }
}

void AnalysisTabW::open_output_folder() {
    const auto* info = d->current_session();
    if (!info) { return; }
    QDesktopServices::openUrl(QUrl::fromLocalFile(info->path + "/anonymized"));
}

} // namespace mosaic
