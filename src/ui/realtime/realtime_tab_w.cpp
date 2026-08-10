#include "ui/realtime/realtime_tab_w.hpp"
#include "ui/anim_utils.hpp"
#include "ui/audio/audio_waveform_w.hpp"
#include "ui/realtime/realtime_camera_tile_w.hpp"
#include "ui/realtime/realtime_trace_w.hpp"
#include "ui/realtime/transcript_panel_w.hpp"
#include <QComboBox>
#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>
#include <algorithm>

namespace mosaic {

namespace {

// Mirrors src/qml/MonitorView.qml's per-camera-count grid-column heuristic
// (n<=1:1, n<=4:2, n<=6:3, else 4) so the Real-time tab's tile grid reads
// consistently with the Live tab's own camera grid at the same camera
// count. Can't literally share code across QML/C++ — a small, documented
// duplication.
int grid_columns(int cameraCount) {
    if (cameraCount <= 1) { return 1; }
    if (cameraCount <= 4) { return 2; }
    if (cameraCount <= 6) { return 3; }
    return 4;
}

QLabel* make_kpi_label() {
    auto* lbl = new QLabel("--");
    lbl->setStyleSheet("color:#c8c8e0; font-family:monospace; font-size:13px;");
    return lbl;
}

} // namespace

struct RealtimeTabW::Impl {
    AppSettings&       settings;
    VideoManager*      videoMgr         = nullptr;
    AudioManager*      audioMgr         = nullptr;
    RecordManager*     recordMgr        = nullptr;
    PoseWorker*        poseWorker       = nullptr;
    TranscriptWorker*  transcriptWorker = nullptr;

    QLabel*       camerasKpi     = nullptr;
    QLabel*       poseKpi        = nullptr;
    QLabel*       gazeKpi        = nullptr;
    QLabel*       statePill      = nullptr;
    QLabel*       pausedBanner   = nullptr;
    QSplitter*    splitter       = nullptr;
    QGridLayout*  tileGrid       = nullptr;
    QWidget*      tileGridHost   = nullptr;
    AudioWaveformW* waveform     = nullptr;

    // Live position trace (see build_ui()'s "Live Trace" panel) — shows one
    // selected camera's selected keypoint moving over the last ~15s.
    QComboBox*      traceCameraCombo   = nullptr;
    QComboBox*      traceKeypointCombo = nullptr;
    RealtimeTraceW*  traceW            = nullptr;
    int             traceCameraIndex   = 0;
    QStringList     traceKeypointNames;   // last-seen names for the selected camera

    // Live speech transcript (see build_ui()'s "TRANSCRIPT" panel) — mic 0
    // only for v1, see TranscriptWorker's own doc comment for why.
    TranscriptPanelW* transcriptW = nullptr;

    QVector<RealtimeCameraTileW*> tiles;
    int  lastCameraCount = -1;
    bool paused          = false;

    explicit Impl(AppSettings& s) : settings(s) {}
};

RealtimeTabW::RealtimeTabW(AppSettings& settings, VideoManager* videoMgr,
                            AudioManager* audioMgr, RecordManager* recordMgr,
                            PoseWorker* poseWorker, TranscriptWorker* transcriptWorker,
                            QWidget* parent)
    : QWidget(parent), d(std::make_unique<Impl>(settings))
{
    d->videoMgr         = videoMgr;
    d->audioMgr         = audioMgr;
    d->recordMgr        = recordMgr;
    d->poseWorker       = poseWorker;
    d->transcriptWorker = transcriptWorker;

    build_ui();
    rebuild_tiles();

    if (d->transcriptWorker) {
        connect(d->transcriptWorker, &TranscriptWorker::transcript_final, this,
                [this](int micIdx, QString text) {
            if (micIdx == 0) { d->transcriptW->push_final(text); }
        }, Qt::QueuedConnection);
        connect(d->transcriptWorker, &TranscriptWorker::transcript_partial, this,
                [this](int micIdx, QString text) {
            if (micIdx == 0) { d->transcriptW->set_tentative(text); }
        }, Qt::QueuedConnection);
    }

    if (d->videoMgr) {
        connect(d->videoMgr, &VideoManager::frame_preview, this,
                [this](int camIdx, QImage frame) {
            if (camIdx >= 0 && camIdx < d->tiles.size()) {
                d->tiles[camIdx]->on_frame_preview(std::move(frame));
            }
        }, Qt::QueuedConnection);
    }

    if (d->poseWorker) {
        connect(d->poseWorker, &PoseWorker::pose_ready, this,
                [this](int camIdx, QVariantList keypoints) {
            on_pose_ready_for_trace(camIdx, keypoints);
            if (camIdx >= 0 && camIdx < d->tiles.size()) {
                d->tiles[camIdx]->on_pose_ready(std::move(keypoints));
            }
        }, Qt::QueuedConnection);
        connect(d->poseWorker, &PoseWorker::gaze_ready, this,
                [this](int camIdx, QVariantMap gazeData) {
            if (camIdx >= 0 && camIdx < d->tiles.size()) {
                d->tiles[camIdx]->on_gaze_ready(std::move(gazeData));
            }
        }, Qt::QueuedConnection);
    }

    if (d->audioMgr && d->waveform) {
        connect(d->audioMgr, &AudioManager::envelope_changed,
                d->waveform, &AudioWaveformW::push_envelope);
    }

    if (d->recordMgr) {
        connect(d->recordMgr, &RecordManager::recording_started, this,
                [this](const QString& path) { on_recording_started(path); });
        connect(d->recordMgr, &RecordManager::recording_stopped, this,
                [this](const QString& path, int durationMs) { on_recording_stopped(path, durationMs); });
        if (d->recordMgr->is_recording()) { on_recording_started(QString()); }
    }

    auto* timer = new QTimer(this);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, &RealtimeTabW::on_tick);
    timer->start();
    on_tick();
}

RealtimeTabW::~RealtimeTabW() = default;

void RealtimeTabW::build_ui() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    // ── KPI strip ────────────────────────────────────────────────────────
    auto* kpiRow = new QHBoxLayout;
    const auto add_kpi = [&](const QString& caption, QLabel*& valueOut) {
        auto* box = new QVBoxLayout;
        auto* cap = new QLabel(caption);
        cap->setStyleSheet("color:#7070a0; font-size:10px; letter-spacing:0.03em;");
        valueOut = make_kpi_label();
        box->addWidget(cap);
        box->addWidget(valueOut);
        kpiRow->addLayout(box);
        kpiRow->addSpacing(24);
    };
    add_kpi("CAMERAS ACTIVE", d->camerasKpi);
    add_kpi("AVG POSE DETECTION", d->poseKpi);
    add_kpi("AVG GAZE ON-TARGET", d->gazeKpi);
    kpiRow->addStretch(1);
    d->statePill = new QLabel("● Live");
    d->statePill->setStyleSheet(
        "QLabel { color:#44cc88; background:rgba(68,204,136,0.15); "
        "border:1px solid #44cc88; border-radius:4px; padding:3px 10px; font-weight:600; }");
    kpiRow->addWidget(d->statePill);
    root->addLayout(kpiRow);

    // ── Live Trace + Audio waveform, side by side ─────────────────────────
    // Shared panel chrome so the two read as a matched pair, not two
    // independently-styled widgets bolted together.
    const auto make_panel = [](const QString& title, QWidget** headerRowHost) -> QWidget* {
        auto* box = new QWidget;
        box->setStyleSheet("background:#13132a; border:1px solid #252545; border-radius:6px;");
        auto* lay = new QVBoxLayout(box);
        lay->setContentsMargins(10, 8, 10, 10);
        lay->setSpacing(6);

        auto* header = new QWidget;
        auto* headerLay = new QHBoxLayout(header);
        headerLay->setContentsMargins(0, 0, 0, 0);
        auto* titleLbl = new QLabel(title);
        titleLbl->setStyleSheet("color:#7070a0; font-size:10px; letter-spacing:0.03em;");
        headerLay->addWidget(titleLbl);
        headerLay->addStretch(1);
        lay->addWidget(header);
        if (headerRowHost) { *headerRowHost = header; }
        return box;
    };

    QWidget* traceHeader = nullptr;
    auto* traceBox = make_panel("LIVE TRACE", &traceHeader);
    auto* traceHeaderLay = qobject_cast<QHBoxLayout*>(traceHeader->layout());
    d->traceCameraCombo = new QComboBox;
    d->traceCameraCombo->setStyleSheet("font-size:11px;");
    d->traceKeypointCombo = new QComboBox;
    d->traceKeypointCombo->setStyleSheet("font-size:11px;");
    d->traceKeypointCombo->setMinimumWidth(120);
    traceHeaderLay->addWidget(d->traceCameraCombo);
    traceHeaderLay->addWidget(d->traceKeypointCombo);
    d->traceW = new RealtimeTraceW;
    static_cast<QVBoxLayout*>(traceBox->layout())->addWidget(d->traceW);

    connect(d->traceCameraCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (idx < 0) { return; }
        d->traceCameraIndex = idx;
        d->traceKeypointNames.clear();
        d->traceKeypointCombo->clear();
        d->traceW->clear();
    });
    connect(d->traceKeypointCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        d->traceW->clear();
    });

    auto* audioBox = make_panel("AUDIO", nullptr);
    d->waveform = new AudioWaveformW;
    d->waveform->set_channel_count(
        std::max(1, static_cast<int>(d->settings.audio.microphones.size())));
    static_cast<QVBoxLayout*>(audioBox->layout())->addWidget(d->waveform);

    auto* transcriptBox = make_panel("TRANSCRIPT · mic 0", nullptr);
    d->transcriptW = new TranscriptPanelW;
    static_cast<QVBoxLayout*>(transcriptBox->layout())->addWidget(d->transcriptW);
    if (!d->transcriptWorker) {
        d->transcriptW->set_unavailable(
            "Live transcript unavailable — analysis/.venv or "
            "analysis/run_live_transcribe.py not found.");
    }

    auto* topRow = new QWidget;
    auto* topRowLay = new QHBoxLayout(topRow);
    topRowLay->setContentsMargins(0, 0, 0, 0);
    topRowLay->setSpacing(8);
    topRowLay->addWidget(traceBox, 2);
    topRowLay->addWidget(audioBox, 1);
    topRowLay->addWidget(transcriptBox, 2);

    // ── Paused banner (hidden until a recording starts) ─────────────────
    d->pausedBanner = new QLabel(
        "⏸  Analysis paused — recording in progress");
    d->pausedBanner->setStyleSheet(
        "QLabel { color:#ddaa44; background:rgba(221,170,68,0.15); "
        "border:1px solid #ddaa44; border-radius:4px; padding:6px 10px; font-weight:600; }");
    d->pausedBanner->hide();
    root->addWidget(d->pausedBanner);

    // ── [Trace | Audio] row + tile grid, in a resettable splitter ─────────
    d->splitter = new QSplitter(Qt::Vertical);
    d->splitter->setHandleWidth(3);
    d->splitter->addWidget(topRow);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    d->tileGridHost = new QWidget;
    d->tileGrid = new QGridLayout(d->tileGridHost);
    d->tileGrid->setSpacing(10);
    scroll->setWidget(d->tileGridHost);
    d->splitter->addWidget(scroll);

    d->splitter->setStretchFactor(0, 0);
    d->splitter->setStretchFactor(1, 1);
    root->addWidget(d->splitter, 1);

    reset_layout();
}

void RealtimeTabW::reset_layout() {
    const int total = std::max(400, height());
    d->splitter->setSizes({260, total - 260});
}

void RealtimeTabW::rebuild_tiles() {
    const int count = static_cast<int>(d->settings.video.cameras.size());
    d->lastCameraCount = count;

    // Detach each old tile from the grid before scheduling its deletion —
    // deleteLater() alone leaves its QLayoutItem occupying its cell until
    // the event loop actually destroys it, which would collide with the new
    // tile added to that same cell immediately below (matches
    // PerformanceMonitorW::rebuild_camera_rows()'s explicit remove-then-
    // delete convention for the same reason).
    for (auto* tile : d->tiles) {
        d->tileGrid->removeWidget(tile);
        tile->deleteLater();
    }
    d->tiles.clear();

    const int cols = grid_columns(count);
    for (int i = 0; i < count; ++i) {
        const bool enabled = d->settings.video.cameras[static_cast<size_t>(i)].liveAnalysisEnabled;
        auto* tile = new RealtimeCameraTileW(i, enabled, d->settings.realtime.detectionWindowBuckets,
                                              d->tileGridHost);
        connect(tile, &RealtimeCameraTileW::analyze_toggled, this,
                [this](int camIdx, bool on) {
            if (camIdx >= 0 && camIdx < static_cast<int>(d->settings.video.cameras.size())) {
                d->settings.video.cameras[static_cast<size_t>(camIdx)].liveAnalysisEnabled = on;
            }
        });
        tile->set_analysis_paused(d->paused);
        d->tileGrid->addWidget(tile, i / cols, i % cols);
        d->tiles.append(tile);
    }

    rebuild_trace_camera_combo();
}

void RealtimeTabW::rebuild_trace_camera_combo() {
    const int count = static_cast<int>(d->settings.video.cameras.size());
    const int previous = d->traceCameraCombo->count() > 0
        ? d->traceCameraCombo->currentIndex() : 0;

    const QSignalBlocker blocker(d->traceCameraCombo);
    d->traceCameraCombo->clear();
    for (int i = 0; i < count; ++i) { d->traceCameraCombo->addItem(QString("Cam %1").arg(i)); }

    const int restored = std::clamp(previous, 0, std::max(0, count - 1));
    d->traceCameraCombo->setCurrentIndex(count > 0 ? restored : -1);
    d->traceCameraIndex = count > 0 ? restored : -1;
}

void RealtimeTabW::on_pose_ready_for_trace(int camIdx, const QVariantList& keypoints) {
    if (camIdx != d->traceCameraIndex || keypoints.isEmpty()) { return; }

    // (Re)populate the keypoint combo the first time this camera's data
    // names a set of keypoints, or if the set itself ever changes —
    // preserves the current selection by name where it still exists,
    // mirroring AnalysisTabW's own keypointCombo-repopulation convention.
    QStringList names;
    names.reserve(keypoints.size());
    for (const auto& kp : keypoints) { names << kp.toMap().value("name").toString(); }
    if (names != d->traceKeypointNames) {
        const QString previous = d->traceKeypointCombo->currentText();
        const QSignalBlocker blocker(d->traceKeypointCombo);
        d->traceKeypointCombo->clear();
        d->traceKeypointCombo->addItems(names);
        const int restoreIdx = names.indexOf(previous);
        d->traceKeypointCombo->setCurrentIndex(restoreIdx >= 0 ? restoreIdx : 0);
        d->traceKeypointNames = names;
    }

    const int selected = d->traceKeypointCombo->currentIndex();
    if (selected < 0 || selected >= keypoints.size()) { return; }
    const auto kp = keypoints[selected].toMap();
    // Same 0.4 visibility cutoff RealtimeCameraTileW's own overlay already
    // uses (realtime_camera_tile_w.cpp) — a low-confidence detection is
    // skipped rather than plotted as if it were real, matching this
    // codebase's established "skip missing samples" discipline.
    if (kp.value("visibility").toDouble() < 0.4) { return; }
    d->traceW->push_sample(QDateTime::currentMSecsSinceEpoch(),
                            kp.value("x").toDouble(), kp.value("y").toDouble());
}

void RealtimeTabW::on_tick() {
    const int count = static_cast<int>(d->settings.video.cameras.size());
    if (count != d->lastCameraCount) { rebuild_tiles(); }

    int    activeCount = 0;
    double poseSum = 0.0; int poseN = 0;
    double gazeSum = 0.0; int gazeN = 0;
    for (auto* tile : d->tiles) {
        if (const auto r = tile->detection_rate()) {
            ++activeCount;
            poseSum += *r; ++poseN;
        }
        if (const auto g = tile->gaze_on_target_rate()) {
            gazeSum += *g; ++gazeN;
        }
    }

    d->camerasKpi->setText(QString("%1/%2").arg(activeCount).arg(d->tiles.size()));
    d->poseKpi->setText(poseN > 0 ? QString("%1%").arg(qRound(poseSum / poseN * 100))
                                   : QStringLiteral("--"));
    d->gazeKpi->setText(gazeN > 0 ? QString("%1%").arg(qRound(gazeSum / gazeN * 100))
                                   : QStringLiteral("--"));

    // Forces the trace to re-trim/repaint against real wall-clock time even
    // between pose samples, so it visibly scrolls/empties out if its camera
    // goes quiet rather than only updating on push_sample().
    if (d->traceW) { d->traceW->update(); }
}

void RealtimeTabW::on_recording_started(const QString&) {
    if (d->paused) { return; }
    d->paused = true;
    d->statePill->setText("⏸ Paused");
    d->statePill->setStyleSheet(
        "QLabel { color:#ddaa44; background:rgba(221,170,68,0.15); "
        "border:1px solid #ddaa44; border-radius:4px; padding:3px 10px; font-weight:600; }");
    d->pausedBanner->show();
    anim::shake_widget(d->pausedBanner);
    for (auto* tile : d->tiles) { tile->set_analysis_paused(true); }
}

void RealtimeTabW::on_recording_stopped(const QString&, int) {
    if (!d->paused) { return; }
    d->paused = false;
    d->statePill->setText("● Live");
    d->statePill->setStyleSheet(
        "QLabel { color:#44cc88; background:rgba(68,204,136,0.15); "
        "border:1px solid #44cc88; border-radius:4px; padding:3px 10px; font-weight:600; }");
    d->pausedBanner->hide();
    for (auto* tile : d->tiles) { tile->set_analysis_paused(false); }
}

} // namespace mosaic
