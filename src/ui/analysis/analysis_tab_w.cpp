#include "ui/analysis/analysis_tab_w.hpp"

#include <QAbstractItemView>
#include <QBrush>
#include <QChart>
#include <QChartView>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEasingCurve>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLegend>
#include <QLegendMarker>
#include <QLineEdit>
#include <QLineSeries>
#include <QListWidget>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTextEdit>
#include <QTextStream>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QValueAxis>
#include <QVariantAnimation>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

#include "analysis/dyadic_kinematics.hpp"
#include "analysis/expression_result.hpp"
#include "analysis/gaze2d_result.hpp"
#include "analysis/gaze_fusion_result.hpp"
#include "analysis/pose_analysis_result.hpp"
#include "analysis/pose_kinematics.hpp"
#include "analysis/pose_models.hpp"
#include "analysis/realtime_metrics.hpp"
#include "analysis/rppg_result.hpp"
#include "analysis/skeleton3d_result.hpp"
#include "analysis/transcript_result.hpp"
#include "analysis/trigger_frame_map.hpp"
#include "audio/audio_envelope.hpp"
#include "session/session_info.hpp"
#include "ui/analysis/gaze_room_view_w.hpp"
#include "ui/analysis/pose_overlay_player_w.hpp"
#include "ui/analysis/skeleton3d_room_view_w.hpp"
#include "ui/analysis/subject_colors.hpp"
#include "ui/anim_utils.hpp"
#include "ui/audio/audio_waveform_w.hpp"
#include "ui/calibration/badge_style.hpp"

namespace mosaic {

namespace {

// Shared by export_kinematics_csv()/export_transcript_csv() (and, in spirit,
// SessionBrowserW::export_annot_csv()) — the QFileDialog/QFile/QTextStream
// save-as skeleton is otherwise duplicated verbatim at every CSV export site
// in the app. Returns false (no file written) if the user cancels the
// dialog or the chosen path can't be opened for writing.
bool export_csv(QWidget* parent, const QString& dialogTitle, const QString& suggestedPath,
                const std::function<void(QTextStream&)>& writeBody) {
    const QString dst =
        QFileDialog::getSaveFileName(parent, dialogTitle, suggestedPath, "CSV files (*.csv)");
    if (dst.isEmpty()) {
        return false;
    }

    QFile f(dst);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream ts(&f);
    writeBody(ts);
    return true;
}

// Used by transcript_json_path_for() — "truncate at the last dot, append a
// fixed suffix" for a sidecar living right beside its source file. Pose and
// Expression output moved to their own dedicated subfolders (pose_dir/
// expression_dir) and no longer use this convention — see
// pose_json_path_for()/expression_json_path_for() below.
QString sidecar_path_for(const QString& relPath, const QString& suffix) {
    QString base  = relPath;
    const int dot = base.lastIndexOf('.');
    if (dot >= 0) {
        base.truncate(dot);
    }
    return base + suffix;
}

// Reads a WAV file's PCM data (16-bit only — the only format WavWriter ever
// produces) and reduces it to `numColumns` (min, max) envelope pairs spanning
// the whole clip, for AudioWaveformW::set_static_envelope(). Streams the file
// one column's worth of bytes at a time via compute_envelope() rather than
// loading the whole clip into memory. Returns an empty result (durationMs=0,
// columns empty) for anything that isn't a well-formed 16-bit PCM WAV.
struct WavEnvelopeResult {
    QVector<QPair<float, float>> columns;
    qint64 durationMs = 0;
};

WavEnvelopeResult load_wav_envelope(const QString& wavPath, int numColumns) {
    WavEnvelopeResult result;
    QFile f(wavPath);
    if (!f.open(QIODevice::ReadOnly)) {
        return result;
    }

    const QByteArray riffHeader = f.read(12);
    if (riffHeader.size() != 12 || riffHeader.mid(0, 4) != "RIFF" ||
        riffHeader.mid(8, 4) != "WAVE") {
        return result;
    }

    int channels      = 0;
    int sampleRate    = 0;
    int bitsPerSample = 0;
    qint64 dataOffset = -1;
    qint64 dataSize   = 0;

    while (!f.atEnd()) {
        const QByteArray chunkId   = f.read(4);
        const QByteArray sizeBytes = f.read(4);
        if (chunkId.size() != 4 || sizeBytes.size() != 4) {
            break;
        }
        const quint32 chunkSize = quint8(sizeBytes[0]) | (quint8(sizeBytes[1]) << 8) |
                                  (quint8(sizeBytes[2]) << 16) |
                                  (quint32(quint8(sizeBytes[3])) << 24);
        const qint64 chunkDataStart = f.pos();

        if (chunkId == "fmt " && chunkSize >= 16) {
            const QByteArray fmt = f.read(16);
            if (fmt.size() < 16) {
                return result;
            }
            channels   = quint8(fmt[2]) | (quint8(fmt[3]) << 8);
            sampleRate = quint8(fmt[4]) | (quint8(fmt[5]) << 8) | (quint8(fmt[6]) << 16) |
                         (quint32(quint8(fmt[7])) << 24);
            bitsPerSample = quint8(fmt[14]) | (quint8(fmt[15]) << 8);
        } else if (chunkId == "data") {
            dataOffset = chunkDataStart;
            dataSize   = chunkSize;
        }
        // RIFF chunks are word-aligned — an odd-sized chunk has one pad byte.
        if (!f.seek(chunkDataStart + chunkSize + (chunkSize % 2))) {
            break;
        }
    }

    if (dataOffset < 0 || channels <= 0 || sampleRate <= 0 || bitsPerSample != 16 ||
        dataSize <= 0) {
        return result;
    }

    const qint64 frameSize   = 2 * channels;
    const qint64 totalFrames = dataSize / frameSize;
    if (totalFrames <= 0) {
        return result;
    }
    result.durationMs = totalFrames * 1000 / sampleRate;

    numColumns                   = std::max(1, numColumns);
    const qint64 framesPerColumn = std::max<qint64>(1, totalFrames / numColumns);
    const qint64 bytesPerColumn  = framesPerColumn * frameSize;

    f.seek(dataOffset);
    qint64 remaining = dataSize;
    result.columns.reserve(numColumns);
    while (remaining > 0 && result.columns.size() < numColumns) {
        const QByteArray chunk = f.read(std::min(bytesPerColumn, remaining));
        if (chunk.isEmpty()) {
            break;
        }
        const AudioEnvelope env = compute_envelope(chunk.constData(), chunk.size());
        result.columns.append({env.minSample, env.maxSample});
        remaining -= chunk.size();
    }
    return result;
}

// Detected subject count for a Pose result: the widest "subjects" array
// seen in any single frame. Not a claim of tracked identity across frames
// (see AnalysisTabW::rebuild_subject_chips()'s own doc comment) — just
// "how many chips to offer".
int max_subjects_in(const PoseAnalysisResult& result) {
    int maxSubjects = 0;
    for (const auto& frame : result.frames()) {
        maxSubjects = std::max(maxSubjects, static_cast<int>(frame.subjects.size()));
    }
    return maxSubjects;
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
        chart->setBackgroundBrush(QColor("#0e0e22"));
        chart->setBackgroundPen(QPen(QColor("#252545"), 1));
        chart->setBackgroundRoundness(10);
        chart->setMargins(QMargins(10, 10, 10, 6));
        chart->setAnimationOptions(QChart::SeriesAnimations);

        chart->legend()->setVisible(true);
        chart->legend()->setAlignment(Qt::AlignBottom);
        chart->legend()->setBackgroundVisible(false);
        chart->legend()->setLabelColor(QColor("#a0a0c8"));
        // MarkerShapeDefault (the implicit default) renders every legend
        // swatch as a plain solid-color rectangle, regardless of the
        // series' own QPen — so same-color solid/dashed X/Y series (see
        // set_multi_subject_position()) were indistinguishable by anything
        // but their text label. MarkerShapeFromSeries draws each swatch as
        // a short line segment using that series' real pen, dash pattern
        // included.
        chart->legend()->setMarkerShape(QLegend::MarkerShapeFromSeries);
        QFont legendFont;
        legendFont.setPointSize(10);
        chart->legend()->setFont(legendFont);

        QFont chartTitleFont;
        chartTitleFont.setPointSize(13);
        chartTitleFont.setBold(true);
        chart->setTitleFont(chartTitleFont);
        chart->setTitleBrush(QColor("#d8d8f0"));
        chart->setTitle("Select a session and camera to begin");

        setChart(chart);
        setRenderHint(QPainter::Antialiasing);
        setStyleSheet("background: transparent; border: none;");

        xSeries_ = new QLineSeries();
        xSeries_->setName("X position");
        xSeries_->setPen(QPen(QColor("#44aaff"), 2));
        ySeries_ = new QLineSeries();
        ySeries_->setName("Y position");
        ySeries_->setPen(QPen(QColor("#ffaa44"), 2));
        // Single-metric series (Speed/Acceleration/Score) — kept separate
        // from xSeries_/ySeries_ rather than repurposing one of them, so
        // Position mode's two-line plot and single-metric mode's one-line
        // plot never fight over which series is "the x-position line". Name
        // is set dynamically per call (set_single_series()) since it stands
        // for a different metric depending on which plugin/mode is active.
        valueSeries_ = new QLineSeries();
        valueSeries_->setPen(QPen(QColor("#44e0aa"), 2));
        // Deliberately excluded from the legend (see the marker-hiding loop
        // below) — a dashed vertical line at "the current playback time" is
        // self-explanatory on sight and doesn't need a legend entry.
        playheadSeries_ = new QLineSeries();
        playheadSeries_->setPen(QPen(QColor("#ff4466"), 1, Qt::DashLine));

        chart->addSeries(xSeries_);
        chart->addSeries(ySeries_);
        chart->addSeries(valueSeries_);
        chart->addSeries(playheadSeries_);

        // setChart() above already ran, so chart() is safe to use here —
        // reuses the same helper set_data()/set_single_series() call
        // instead of a second copy of the same marker-hiding loop.
        set_series_marker_visible(playheadSeries_, false);

        axisX_ = new QValueAxis();
        axisX_->setLabelFormat("%.1f");
        axisX_->setTitleText("Time (s)");
        axisX_->setLabelsColor(QColor("#7070a0"));
        axisX_->setTitleBrush(QColor("#7070a0"));
        axisX_->setGridLineColor(QColor("#181838"));
        axisX_->setRange(0, 1);

        axisY_ = new QValueAxis();
        axisY_->setTitleText("Position (px)");
        axisY_->setLabelsColor(QColor("#7070a0"));
        axisY_->setTitleBrush(QColor("#7070a0"));
        axisY_->setGridLineColor(QColor("#181838"));
        axisY_->setRange(0, 1080);

        // Readability pass: Qt Charts' built-in axis font/line defaults are
        // small and unstyled — set an explicit, larger tick/title font and
        // a visibly thicker spine (matching the app's panel-border color
        // used everywhere else, #252545) instead of leaving the axis line
        // unset with only the grid lines colored.
        QFont tickFont;
        tickFont.setPointSize(11);
        axisX_->setLabelsFont(tickFont);
        axisY_->setLabelsFont(tickFont);

        QFont titleFont;
        titleFont.setPointSize(12);
        titleFont.setBold(true);
        axisX_->setTitleFont(titleFont);
        axisY_->setTitleFont(titleFont);

        const QPen spinePen(QColor("#252545"), 3);
        axisX_->setLinePen(spinePen);
        axisY_->setLinePen(spinePen);

        chart->addAxis(axisX_, Qt::AlignBottom);
        chart->addAxis(axisY_, Qt::AlignLeft);
        for (auto* series : {xSeries_, ySeries_, valueSeries_, playheadSeries_}) {
            series->attachAxis(axisX_);
            series->attachAxis(axisY_);
        }

        // Hover tooltip on each data series — lets a user read an exact
        // value off the curve without needing to click-seek to that exact
        // frame first.
        for (auto* series : {xSeries_, ySeries_, valueSeries_}) {
            connect(series, &QLineSeries::hovered, this,
                    [series](const QPointF& point, bool state) {
                        if (!state) {
                            QToolTip::hideText();
                            return;
                        }
                        QToolTip::showText(
                            QCursor::pos(),
                            QString("%1s, %2").arg(point.x(), 0, 'f', 2).arg(point.y(), 0, 'f', 1));
                    });
        }
    }

    void set_seek_callback(SeekCb cb) { seekCb_ = std::move(cb); }

    // Shown above the plot area — callers build something like
    // "nose — Speed" or a blendshape name so the plot is self-explanatory
    // without needing to cross-reference the controls above it.
    void set_title(const QString& text) { chart()->setTitle(text); }

    // Position mode, one or more subjects: 2 lines per subject (solid = X,
    // dashed = Y), each pair tinted by subject_color(subjectIndex) so a
    // subject's trace here matches its skeleton's color in the video
    // overlay (SkeletonOverlayW's pose branch). Replaces the old single-
    // subject set_data() — subjectIndices.size()==1 (today's default,
    // subject 0 only) reproduces exactly what set_data() used to show,
    // just with subject-aware legend labels instead of plain "X position"/
    // "Y position".
    void set_multi_subject_position(const PoseAnalysisResult& result, int keypointIndex,
                                    const QVector<int>& subjectIndices) {
        clear_all_series();
        clear_subject_series();
        axisY_->setTitleText("Position (px)");
        set_series_marker_visible(valueSeries_, false);
        // xSeries_/ySeries_ are legacy, no-longer-populated series kept only
        // as a stable target for the cross-clearing/marker-hiding calls in
        // set_multi_subject_series()/set_single_series() — hidden here too
        // so their generic "X position"/"Y position" legend entries don't
        // show alongside the real per-subject "Subject N · X/Y" ones (this
        // method runs first, since Position is metricCombo's default).
        set_series_marker_visible(xSeries_, false);
        set_series_marker_visible(ySeries_, false);

        if (!result.is_valid() || result.frames().isEmpty() || keypointIndex < 0 ||
            subjectIndices.isEmpty()) {
            apply_ranges(false, 0.0, 0.0, 0.0);
            return;
        }

        const int64_t t0 = result.frames().first().timestampNs;
        double minY      = std::numeric_limits<double>::max();
        double maxY      = std::numeric_limits<double>::lowest();
        double maxT      = 0.0;

        for (int subjectIndex : subjectIndices) {
            auto* xs = new QLineSeries();
            xs->setName(QString("Subject %1 · X").arg(subjectIndex + 1));
            xs->setPen(QPen(subject_color(subjectIndex), 2, Qt::SolidLine));
            auto* ys = new QLineSeries();
            ys->setName(QString("Subject %1 · Y").arg(subjectIndex + 1));
            ys->setPen(QPen(subject_color(subjectIndex), 2, Qt::DashLine));

            for (const auto& frame : result.frames()) {
                if (subjectIndex >= frame.subjects.size()) {
                    continue;
                }
                const auto& subject = frame.subjects[subjectIndex];
                if (keypointIndex >= subject.keypoints.size()) {
                    continue;
                }

                const double tSec = (frame.timestampNs - t0) / 1e9;
                const auto& kp    = subject.keypoints[keypointIndex];
                xs->append(tSec, kp.x());
                ys->append(tSec, kp.y());

                maxT = std::max(maxT, tSec);
                minY = std::min({minY, kp.x(), kp.y()});
                maxY = std::max({maxY, kp.x(), kp.y()});
            }
            add_subject_series(xs);
            add_subject_series(ys);
        }

        apply_ranges(minY <= maxY, minY, maxY, maxT);
    }

    // Speed/Acceleration mode, one or more subjects: 1 line per subject,
    // tinted by subject_color(subjectIndex). perSubject: (subjectIndex,
    // points) pairs, points already in (ms-since-start, value) form
    // (caller pre-filters NaN, exactly as the old single-subject code
    // already did). Same minDurationMs floor behavior as
    // set_single_series() — see that method's own doc comment.
    void set_multi_subject_series(const QVector<QPair<int, QVector<QPointF>>>& perSubject,
                                  const QString& yAxisLabel, double minDurationMs = 0.0) {
        clear_all_series();
        clear_subject_series();
        axisY_->setTitleText(yAxisLabel);
        set_series_marker_visible(xSeries_, false);
        set_series_marker_visible(ySeries_, false);
        set_series_marker_visible(valueSeries_, false);

        if (perSubject.isEmpty()) {
            apply_ranges(false, 0.0, 0.0, 0.0);
            return;
        }

        double minY = std::numeric_limits<double>::max();
        double maxY = std::numeric_limits<double>::lowest();
        double maxT = std::max(0.0, minDurationMs) / 1000.0;

        for (const auto& [subjectIndex, points] : perSubject) {
            auto* s = new QLineSeries();
            s->setName(QString("Subject %1").arg(subjectIndex + 1));
            s->setPen(QPen(subject_color(subjectIndex), 2));
            for (const auto& p : points) {
                const double tSec = p.x() / 1000.0;
                s->append(tSec, p.y());
                maxT = std::max(maxT, tSec);
                minY = std::min(minY, p.y());
                maxY = std::max(maxY, p.y());
            }
            add_subject_series(s);
        }

        apply_ranges(minY <= maxY, minY, maxY, maxT);
    }

    // Single-metric mode (Speed/Acceleration/expression score/…): points
    // are (ms-since-start, value), pre-filtered by the caller to drop NaN
    // entries (a NaN plotted point would otherwise break QLineSeries' range
    // calculation) — converted to seconds internally to match axisX_.
    // Clears xSeries_/ySeries_ so Position mode's two-line plot doesn't
    // linger underneath. seriesName (shown in the legend) defaults to
    // yAxisLabel when not given.
    //
    // minDurationMs: the X axis always extends at least this far, even past
    // the last plotted point. Needed because a caller's points can stop
    // short of the actual analyzed range — e.g. Facial Expression only
    // plots frames where a face was detected, but the video (and the
    // analysis) continues past wherever detection happens to drop out —
    // without this floor the axis silently truncated at the last detection
    // instead of spanning the video's real analyzed duration, visibly
    // mismatching the player's own duration/scrubber. Ignored when points
    // is empty (that "no data at all" case already resets to a clean (0,1)
    // default below and shouldn't be forced into a wide, data-less range).
    void set_single_series(const QVector<QPointF>& points, const QString& yAxisLabel,
                           const QString& seriesName = QString(), double minDurationMs = 0.0) {
        clear_all_series();
        clear_subject_series(); // defensive: this widget is also reused by Facial
                                // Expression, which never populates subjectSeries_
                                // itself, but a stale entry from an earlier Pose
                                // view shouldn't be able to linger regardless.
        axisY_->setTitleText(yAxisLabel);
        valueSeries_->setName(seriesName.isEmpty() ? yAxisLabel : seriesName);
        set_series_marker_visible(xSeries_, false);
        set_series_marker_visible(ySeries_, false);
        set_series_marker_visible(valueSeries_, true);

        if (points.isEmpty()) {
            apply_ranges(false, 0.0, 0.0, 0.0);
            return;
        }

        double minY = std::numeric_limits<double>::max();
        double maxY = std::numeric_limits<double>::lowest();
        double maxT = std::max(0.0, minDurationMs) / 1000.0;

        for (const auto& p : points) {
            const double tSec = p.x() / 1000.0;
            valueSeries_->append(tSec, p.y());
            maxT = std::max(maxT, tSec);
            minY = std::min(minY, p.y());
            maxY = std::max(maxY, p.y());
        }

        apply_ranges(true, minY, maxY, maxT);
    }

    void set_playhead_ms(int64_t ms) {
        const double tSec = ms / 1000.0;
        playheadSeries_->clear();
        playheadSeries_->append(tSec, axisY_->min());
        playheadSeries_->append(tSec, axisY_->max());
    }

   protected:
    void mousePressEvent(QMouseEvent* event) override {
        // xSeries_/ySeries_ are no longer ever populated (Position mode now
        // goes through subjectSeries_ — see set_multi_subject_position()),
        // so the coordinate-mapping anchor must be whichever series
        // actually has data: subjectSeries_ (Pose, both modes) if present,
        // else valueSeries_ (Facial Expression's set_single_series() path).
        // mapToValue() only uses the anchor to resolve axis coordinates, so
        // any non-empty series attached to the same axes gives the same
        // answer regardless of which one is passed.
        QLineSeries* anchor = !subjectSeries_.isEmpty() ? subjectSeries_.first() : valueSeries_;
        if (seekCb_ && axisX_->max() > axisX_->min()) {
            const QPointF scenePos = mapToScene(event->pos());
            const QPointF chartPos = chart()->mapFromScene(scenePos);
            const QPointF value    = chart()->mapToValue(chartPos, anchor);
            const int64_t ms =
                static_cast<int64_t>(std::clamp(value.x(), axisX_->min(), axisX_->max()) * 1000.0);
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

    // Removes and deletes every dynamically-created subject series from a
    // previous set_multi_subject_position()/set_multi_subject_series()
    // call — Qt Charts requires an explicit removeSeries() before deleting,
    // and this codebase's own "just rebuild on change, don't diff" idiom
    // (seen elsewhere, e.g. tile-grid rebuilds) applies here too: simpler
    // and safer than trying to reuse/resize existing series across calls
    // with a possibly-different subject count each time.
    void clear_subject_series() {
        for (auto* s : subjectSeries_) {
            chart()->removeSeries(s);
            delete s;
        }
        subjectSeries_.clear();
    }

    // Adds one dynamically-created series: attaches it to both axes, wires
    // the same hover-tooltip behavior every other series already has, and
    // tracks it in subjectSeries_ for the next clear_subject_series() call.
    void add_subject_series(QLineSeries* s) {
        chart()->addSeries(s);
        s->attachAxis(axisX_);
        s->attachAxis(axisY_);
        connect(s, &QLineSeries::hovered, this, [s](const QPointF& point, bool state) {
            if (!state) {
                QToolTip::hideText();
                return;
            }
            QToolTip::showText(QCursor::pos(), QString("%1: %2s, %3")
                                                   .arg(s->name())
                                                   .arg(point.x(), 0, 'f', 2)
                                                   .arg(point.y(), 0, 'f', 1));
        });
        subjectSeries_.push_back(s);
    }

    void set_series_marker_visible(QLineSeries* series, bool visible) {
        for (auto* marker : chart()->legend()->markers(series)) {
            marker->setVisible(visible);
        }
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
            // Matches the constructor's own default (0, 1) — this was
            // (0, 1000) before the axis was converted from milliseconds to
            // seconds, left stale and never updated, producing a
            // ~16.6-minute-wide empty axis for any no-data state (e.g. a
            // freshly-selected camera with no analysis yet).
            axisX_->setRange(0, 1);
            axisY_->setRange(0, 1);
            return;
        }
        axisX_->setRange(0, std::max(maxT, 1.0));
        if (minY <= maxY) {
            const double pad = std::max((maxY - minY) * 0.1, 5.0);
            axisY_->setRange(minY - pad, maxY + pad);
        }
    }

    QLineSeries* xSeries_ = nullptr;        // no longer populated — kept only so
                                            // set_single_series()'s cross-clearing/
                                            // marker-hiding calls have a stable target
    QLineSeries* ySeries_        = nullptr; // (same as xSeries_ above)
    QLineSeries* valueSeries_    = nullptr;
    QLineSeries* playheadSeries_ = nullptr;
    QValueAxis* axisX_           = nullptr;
    QValueAxis* axisY_           = nullptr;
    SeekCb seekCb_;
    // Dynamically created/destroyed per set_multi_subject_position()/
    // set_multi_subject_series() call — see clear_subject_series()/
    // add_subject_series(). Empty when Facial Expression's
    // set_single_series() path is in use.
    QVector<QLineSeries*> subjectSeries_;
};

// Custom-paints session rows with a hover-intensity tint instead of relying
// on default QListWidget/QStyle selection painting. Only one row can be
// hovered at a time in a single view, so this owns a single QVariantAnimation
// (not one per row) driving m_hoverT, keyed against whichever row m_hoveredIndex
// currently names — matching AvatarChip/AddChip's own "animate hover on a
// custom-painted widget via QVariantAnimation + repaint" idiom
// (login_dialog.cpp), the only way to animate hover here since QSS :hover
// transitions aren't supported by Qt's style engine for delegate painting.
class SessionListDelegate : public QStyledItemDelegate {
   public:
    explicit SessionListDelegate(QListWidget* view, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), m_view(view) {
        m_hoverAnim = new QVariantAnimation(this);
        m_hoverAnim->setDuration(120);
        m_hoverAnim->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_hoverAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            m_hoverT = v.toReal();
            if (m_view) {
                m_view->viewport()->update();
            }
        });
        m_view->setMouseTracking(true);
        m_view->viewport()->installEventFilter(this);
    }

    bool eventFilter(QObject* obj, QEvent* event) override {
        if (obj == m_view->viewport()) {
            if (event->type() == QEvent::MouseMove) {
                set_hovered(m_view->indexAt(static_cast<QMouseEvent*>(event)->pos()));
            } else if (event->type() == QEvent::Leave) {
                set_hovered(QModelIndex());
            }
        }
        return QStyledItemDelegate::eventFilter(obj, event);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        const bool isSelected = option.state & QStyle::State_Selected;
        const bool isHovered  = (index == m_hoveredIndex);
        const qreal t         = isHovered ? m_hoverT : 0.0;

        const QColor bg = isSelected ? QColor("#3a3a88")
                                     : anim::lerp_color(QColor("#13132a"), QColor("#1a1a38"), t);
        painter->fillRect(option.rect, bg);

        if (isSelected) {
            painter->fillRect(QRect(option.rect.left(), option.rect.top(), 3, option.rect.height()),
                              QColor("#6060dd"));
        }

        painter->setPen(QColor("#c8c8e0"));
        painter->drawText(option.rect.adjusted(12, 0, -10, 0), Qt::AlignVCenter | Qt::AlignLeft,
                          index.data(Qt::DisplayRole).toString());

        painter->setPen(QColor("#1e1e3a"));
        painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
        painter->restore();
    }

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override {
        QSize s = QStyledItemDelegate::sizeHint(option, index);
        s.setHeight(qMax(s.height(), 28));
        return s;
    }

   private:
    void set_hovered(const QModelIndex& index) {
        if (index == m_hoveredIndex) {
            return;
        }
        m_hoveredIndex = index;
        anim::restart_hover_anim(m_hoverAnim, m_hoverT, index.isValid() ? 1.0 : 0.0);
    }

    QListWidget* m_view            = nullptr;
    QVariantAnimation* m_hoverAnim = nullptr;
    qreal m_hoverT                 = 0.0;
    QPersistentModelIndex m_hoveredIndex;
};

// ── AnalysisTabW::Impl ──────────────────────────────────────────────────

struct AnalysisTabW::Impl {
    AppSettings& settings;
    AnalysisManager* analysisMgr;
    QStringList extraDirectories; // item 27 — admin-only aggregate view

    QListWidget* sessionList = nullptr;
    QComboBox* pluginCombo   = nullptr;

    // Plugin-specific run controls, swapped via controlsStack on plugin change.
    QStackedWidget* controlsStack            = nullptr;
    QComboBox* modelCombo                    = nullptr; // pose
    QSpinBox* skipSpin                       = nullptr; // pose
    QLabel* depthModeHintLbl                 = nullptr; // pose, shown only for depth models
    QComboBox* backendCombo                  = nullptr; // face_mask
    QComboBox* styleCombo                    = nullptr; // face_mask
    QSpinBox* faceSkipSpin                   = nullptr; // face_mask
    QComboBox* whisperModelCombo             = nullptr; // diarize
    QComboBox* languageCombo                 = nullptr; // diarize
    QLineEdit* hfTokenEdit                   = nullptr; // diarize
    QSpinBox* minSpeakersSpin                = nullptr; // diarize
    QSpinBox* maxSpeakersSpin                = nullptr; // diarize
    QCheckBox* skipDiarizationCheck          = nullptr; // diarize
    QComboBox* expressionBackendCombo        = nullptr; // expression
    QSpinBox* maxFacesSpin                   = nullptr; // expression
    QDoubleSpinBox* minConfidenceSpin        = nullptr; // expression
    QSpinBox* exprSkipSpin                   = nullptr; // expression
    QSpinBox* minCamerasSpin                 = nullptr; // gaze_fusion
    QDoubleSpinBox* gazeMinConfidenceSpin    = nullptr; // gaze_fusion
    QSpinBox* gazeSkipSpin                   = nullptr; // gaze_fusion
    QSpinBox* pose3dMinCamerasSpin           = nullptr; // pose3d
    QDoubleSpinBox* maxReprojectionErrorSpin = nullptr; // pose3d
    QSpinBox* pose3dSkipSpin                 = nullptr; // pose3d
    QSpinBox* pose3dSmoothingWindowSpin      = nullptr; // pose3d
    QCheckBox* showSmoothedCheck = nullptr; // pose3d — room view only, see set_show_smoothed()
    QComboBox* rppgBackendCombo  = nullptr; // rppg
    QDoubleSpinBox* rppgWindowSecSpin       = nullptr; // rppg
    QDoubleSpinBox* rppgHopSecSpin          = nullptr; // rppg
    QSpinBox* rppgSmoothingSpin             = nullptr; // rppg
    QDoubleSpinBox* gaze2dMinConfidenceSpin = nullptr; // gaze2d
    QSpinBox* gaze2dSkipSpin                = nullptr; // gaze2d

    QPushButton* runBtn = nullptr;
    QLabel* statusLbl   = nullptr;
    // cameraProgressBar (coarse: which camera out of N, Pose plugin's
    // multi-camera session runs only) sits above progressBar (fine:
    // per-frame % within the camera currently being processed) — both
    // parsed out of output_received, see the "Camera N/M:" vs "NN.N% (a/b)"
    // regexes there.
    QProgressBar* cameraProgressBar = nullptr;
    QProgressBar* progressBar       = nullptr; // per-frame progress, parsed out of output_received
    QTextEdit* logView              = nullptr;

    // Source-picker rows: sourceRowW (Camera/Keypoint, pose+face_mask) and
    // micRowW (Mic, diarize) are mutually exclusive — select_plugin() shows
    // exactly one, mirroring how kinematicsRowW is toggled.
    QWidget* sourceRowW    = nullptr;
    QComboBox* cameraCombo = nullptr; // pose + face_mask + expression + gaze_fusion + pose3d
    // Each combo's own "Label:" is a separate sibling widget in resultsRow,
    // not something select_plugin() can toggle via the combo pointer alone
    // — these *FieldW containers pair each up so one setVisible() call
    // hides both together, matching this file's established
    // own-container-widget convention (kinematicsRowW etc).
    QWidget* keypointFieldW    = nullptr;
    QComboBox* keypointCombo   = nullptr; // pose only
    QWidget* blendshapeFieldW  = nullptr;
    QComboBox* blendshapeCombo = nullptr; // expression only
    QWidget* trackFieldW       = nullptr;
    QComboBox* trackCombo      = nullptr; // pose3d only — filters the CSV export;
                                          // the 3D room view always shows every track
    PoseOverlayPlayerW* player = nullptr; // shared: video (pose/face_mask/expression),
                                          // or audio (diarize)
    QSplitter* resultsSplitter = nullptr; // holds player/chart/transcriptTable/
                                          // roomView/skeleton3dRoomView/triggerSyncTable
    MetricsChartW* chart       = nullptr; // pose + expression
    QPushButton* openFolderBtn = nullptr; // face_mask only

    QWidget* micRowW                 = nullptr; // diarize only
    QComboBox* micCombo              = nullptr; // diarize only
    QLabel* transcriptStatsLbl       = nullptr; // diarize only
    QPushButton* exportTranscriptBtn = nullptr; // diarize only
    QTableWidget* transcriptTable    = nullptr; // diarize only
    AudioWaveformW* diarizeWaveform  = nullptr; // diarize only — whole-clip static waveform,
                                                // playhead-synced to d->player, same visual
                                                // style as the Audio settings tab's live view
    QWidget* speakerLegendRowW = nullptr;       // diarize only — color-swatch legend for the
                                                // waveform's speaker bands, rebuilt by
                                                // rebuild_speaker_legend()

    // Expression view controls — mirrors kinematicsRowW's own-container
    // pattern so select_plugin() can hide the whole row with one call.
    QWidget* expressionRowW          = nullptr; // expression only
    QLabel* expressionStatsLbl       = nullptr; // expression only
    QPushButton* exportExpressionBtn = nullptr; // expression only

    // Gaze-fusion view controls — mirrors expressionRowW/kinematicsRowW's
    // own-container pattern so select_plugin() can hide the whole row with
    // one call. roomView is the 3D top-down view, added as a 4th
    // resultsSplitter child (not part of this row) since it needs the same
    // stretch-factor treatment as player/chart/transcriptTable.
    QWidget* gazeFusionRowW    = nullptr; // gaze_fusion only
    QLabel* gazeStatsLbl       = nullptr; // gaze_fusion only
    QPushButton* exportGazeBtn = nullptr; // gaze_fusion only
    GazeRoomViewW* roomView    = nullptr; // gaze_fusion only

    // 3D Pose Reconstruction view controls — mirrors gazeFusionRowW's
    // own-container pattern. skeleton3dRoomView is the interactive
    // orbit-rotatable 3D view, added as resultsSplitter's 5th child (not
    // part of this row) for the same stretch-factor reason roomView isn't.
    QWidget* pose3dRowW                     = nullptr; // pose3d only
    QLabel* pose3dStatsLbl                  = nullptr; // pose3d only
    QPushButton* exportPose3dBtn            = nullptr; // pose3d only
    Skeleton3DRoomViewW* skeleton3dRoomView = nullptr; // pose3d only

    // Dyad Analysis — a derived-metrics mode added within the 3D Pose
    // Reconstruction plugin's own results view (mirrors item 16's own
    // precedent: Pose Kinematics is a Metric mode inside the Pose plugin,
    // not a separate plugin), since it needs no new Python job/JSON — it's
    // pure C++ math over the already-loaded currentSkeleton3D. Own-container
    // row, same pattern as pose3dRowW. Uses the SAME shared d->chart as
    // every other single-series plugin view (Pose Kinematics/rPPG/
    // Expression/2D Gaze) — see select_plugin()'s d->chart visibility
    // condition, which pose3d must now be included in.
    QWidget* dyadRowW          = nullptr; // pose3d only
    QComboBox* dyadTrackACombo = nullptr; // pose3d only
    QComboBox* dyadTrackBCombo = nullptr; // pose3d only
    QComboBox* dyadMetricCombo = nullptr; // pose3d only
    QSpinBox* dyadWindowSpin   = nullptr; // pose3d only — congruent-motion rolling window
    QLabel* dyadStatsLbl       = nullptr; // pose3d only
    QPushButton* exportDyadBtn = nullptr; // pose3d only

    // EEG/Trigger sync view controls — mirrors pose3dRowW's own-container
    // pattern. No model/backend/skip controls page exists for this plugin
    // (nothing to tune — it's a deterministic CSV-to-CSV lookup), so its
    // controlsStack page is just an informational label.
    QWidget* triggerSyncRowW          = nullptr; // trigger_sync only
    QTableWidget* triggerSyncTable    = nullptr; // trigger_sync only
    QLabel* triggerSyncStatsLbl       = nullptr; // trigger_sync only
    QPushButton* exportTriggerSyncBtn = nullptr; // trigger_sync only

    // Remote Heart Rate (rPPG) view controls — mirrors triggerSyncRowW's
    // own-container pattern. Reuses the shared MetricsChartW (via
    // set_single_series()) for the BPM-over-time trace rather than adding a
    // new chart widget — same reuse this plugin's Approach section commits
    // to. rppgDisclaimerLbl is deliberately a persistent, always-visible
    // banner (not a tooltip) — this plugin's whole safety framing depends
    // on the "experimental, not clinical" caveat never being missable.
    QWidget* rppgRowW         = nullptr;        // rppg only
    QLabel* rppgDisclaimerLbl = nullptr;        // rppg only
    QCheckBox* rppgShowSmoothedCheck = nullptr; // rppg only — toggles which series the chart plots
    QLabel* rppgStatsLbl     = nullptr;         // rppg only
    QLabel* rppgQualityBadge = nullptr;         // rppg only — rppg_quality_for() tier, reusing
                                                // the same RmsQuality/badge_stylesheet() vocabulary
                                                // pose_tracking_quality_for() already established
    QPushButton* exportRppgBtn = nullptr;       // rppg only

    // Calibration-free 2D Gaze view controls — mirrors rppgRowW's
    // own-container pattern. Reuses the shared MetricsChartW (via
    // set_single_series()) for the dx/dy/magnitude-over-time trace, same
    // reuse rppg/pose-kinematics already established. metricCombo below is
    // pose kinematics' own Position/Speed/Acceleration combo — this
    // plugin gets its own separate combo (gaze2dMetricCombo) since its
    // metric choices (dx/dy/magnitude) are a different vocabulary.
    QWidget* gaze2dRowW          = nullptr; // gaze2d only
    QComboBox* gaze2dMetricCombo = nullptr; // gaze2d only — dx / dy / magnitude
    QLabel* gaze2dStatsLbl       = nullptr; // gaze2d only
    QPushButton* exportGaze2dBtn = nullptr; // gaze2d only

    // Kinematics view controls — reshape how the already-loaded
    // currentResult is displayed, not what gets launched (unlike runBox's
    // model/skip controls), so they live in their own row. Pose only —
    // kinematicsRowW lets select_plugin() hide the whole row with one call.
    QWidget* kinematicsRowW          = nullptr;
    QComboBox* metricCombo           = nullptr; // Position / Speed / Acceleration
    QSpinBox* smoothingSpin          = nullptr;
    QDoubleSpinBox* scaleSpin        = nullptr; // mm/px, 1.0 = raw pixels
    QLabel* kinematicsStatsLbl       = nullptr;
    QPushButton* exportKinematicsBtn = nullptr;

    // Subject picker — one checkable chip per detected subject index, so
    // the kinematics chart/overlay can show several subjects' traces at
    // once. Pose only; own-container row so select_plugin() can hide it
    // with one call, matching kinematicsRowW's own convention. Rebuilt by
    // rebuild_subject_chips() whenever a new session/camera loads.
    QWidget* subjectPickerRowW = nullptr;
    QVector<QToolButton*> subjectChips;

    QList<SessionInfo> sessions;
    QString currentSessionPath;
    PoseAnalysisResult currentResult;         // pose only
    TranscriptResult currentTranscript;       // diarize only
    ExpressionResult currentExpressionResult; // expression only
    GazeFusionResult currentGazeFusion;       // gaze_fusion only
    Skeleton3DResult currentSkeleton3D;       // pose3d only
    DyadicKinematicsSeries currentDyad;       // pose3d only — derived from currentSkeleton3D +
                                        // the selected Track A/B pair, see update_dyadic_view()
    TriggerFrameMap currentTriggerFrameMap; // trigger_sync only
    RppgResult currentRppgResult;           // rppg only
    Gaze2dResult currentGaze2dResult;       // gaze2d only

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

    Impl(AppSettings& s, AnalysisManager* mgr, const QStringList& extraDirs = {})
        : settings(s), analysisMgr(mgr), extraDirectories(extraDirs) {}

    [[nodiscard]] const SessionInfo* current_session() const {
        for (const auto& s : sessions) {
            if (s.path == currentSessionPath) {
                return &s;
            }
        }
        return nullptr;
    }

    // Shared by update_kinematics_chart() and export_kinematics_csv() — both
    // need the same "ms since first frame" origin, and both must guard the
    // same empty-frames edge case identically.
    [[nodiscard]] int64_t first_timestamp_ns() const {
        return currentResult.frames().isEmpty() ? 0 : currentResult.frames().first().timestampNs;
    }

    // Called once at the top of reload_current_camera_result() so each
    // plugin branch only needs to (re-)populate the ONE result field it
    // actually owns, instead of every branch separately clearing the other
    // 3-4 fields it doesn't use — a pattern that had drifted into 6+
    // near-identical reset lines across the function.
    void reset_all_results() {
        currentResult           = PoseAnalysisResult();
        currentExpressionResult = ExpressionResult();
        currentTranscript       = TranscriptResult();
        currentGazeFusion       = GazeFusionResult();
        currentSkeleton3D       = Skeleton3DResult();
        currentDyad             = DyadicKinematicsSeries();
        currentTriggerFrameMap  = TriggerFrameMap();
        currentRppgResult       = RppgResult();
        currentGaze2dResult     = Gaze2dResult();
    }
};

// ── Construction ─────────────────────────────────────────────────────────

AnalysisTabW::AnalysisTabW(AppSettings& settings, AnalysisManager* analysisMgr,
                           const QStringList& extraDirectories, QWidget* parent)
    : QWidget(parent), d(std::make_unique<Impl>(settings, analysisMgr, extraDirectories)) {
    build_ui();
    // pluginCombo's first addItem() (build_ui(), "Pose (YOLOv8)") fires its
    // own currentIndexChanged(0) synchronously, before the connect() to
    // select_plugin() a few lines later even runs — so select_plugin() was
    // never actually invoked for the startup-default plugin. Every
    // plugin-specific control that relies solely on select_plugin() to
    // hide it (blendshapeCombo, trackCombo, expressionRowW, gazeFusionRowW,
    // pose3dRowW — everything except micRowW, which has an explicit
    // construction-time setVisible(false)) was left at its default
    // QWidget-visible state: Blendshape/Track combos and 3 unrelated
    // "Export CSV" buttons all showing at once alongside the real Pose
    // controls. Call it explicitly now that every widget it touches exists.
    select_plugin(d->pluginCombo->currentIndex());
    rebuild_session_list();

    connect(analysisMgr, &AnalysisManager::analysis_started, this, [this](const QString& path) {
        d->jobIsMine = (path == d->currentSessionPath);
        d->runBtn->setEnabled(false); // AnalysisManager only runs one job at a time either way
        if (!d->jobIsMine) {
            return;
        }
        d->statusLbl->setText("Running…");
        d->statusLbl->setStyleSheet("color:#ddaa33; font-size:15px; font-weight:600;");
        d->logView->clear();
        d->progressBar->setValue(0);
        d->progressBar->setFormat("%p%");
        d->progressBar->setVisible(false);
        d->cameraProgressBar->setValue(0);
        d->cameraProgressBar->setVisible(false);
    });
    connect(analysisMgr, &AnalysisManager::output_received, this, [this](const QString& line) {
        if (!d->jobIsMine) {
            return;
        }

        // Multi-camera session runs print one "Camera N/M: <file>" banner
        // before each camera's own per-frame ticker starts — a coarser,
        // session-wide sibling to progressRe below. Matches any plugin's own
        // "[run_X]" tag (run_pose.py and run_face_mask.py both print this
        // shape; any future per-camera plugin script gets it for free by
        // following the same convention) rather than hardcoding one script's
        // tag. Still falls through to the log (infrequent — once per camera,
        // not per frame — so no spam concern), unlike the per-frame match
        // which replaces the log line entirely.
        static const QRegularExpression cameraRe(
            QStringLiteral(R"(^\[run_\w+\] Camera (\d+)/(\d+):)"));
        const auto cameraMatch = cameraRe.match(line);
        if (cameraMatch.hasMatch()) {
            const int camIdx   = cameraMatch.captured(1).toInt();
            const int camTotal = cameraMatch.captured(2).toInt();
            d->cameraProgressBar->setVisible(true);
            d->cameraProgressBar->setRange(0, camTotal);
            d->cameraProgressBar->setValue(camIdx);
            d->cameraProgressBar->setFormat(QString("Camera %1/%2").arg(camIdx).arg(camTotal));
            // Each new camera restarts its own per-frame progress at 0 —
            // avoids the bar briefly showing the previous camera's leftover
            // percentage before its first "NN.N% (a/b)" line arrives.
            d->progressBar->setValue(0);
        }

        // Every plugin script prints its per-frame ticker in this same
        // shape (run_pose.py's "  42.7%  (1200/2810)  18.3s elapsed", and
        // sibling scripts that copied its progress-print convention) —
        // route it to the progress bar instead of the log view, so a run
        // shows one live-updating bar instead of a wall of tiny scrolling
        // text. Anything else (start/done/error banners) still goes to the
        // log unchanged.
        static const QRegularExpression progressRe(
            QStringLiteral(R"(^\s*(\d+(?:\.\d+)?)%\s+\((\d+)/(\d+)\))"));
        const auto match = progressRe.match(line);
        if (match.hasMatch()) {
            const double pct = match.captured(1).toDouble();
            d->progressBar->setVisible(true);
            d->progressBar->setValue(static_cast<int>(pct * 10.0));
            d->progressBar->setFormat(QString("%1%  (%2/%3 frames)")
                                          .arg(pct, 0, 'f', 1)
                                          .arg(match.captured(2), match.captured(3)));
            return;
        }
        d->logView->append(line);
    });
    connect(analysisMgr, &AnalysisManager::setup_error, this, [this](const QString& msg) {
        // No session/path info on this signal — always surface it rather
        // than risk hiding a failure of the user's own just-clicked Run.
        d->statusLbl->setText("Error: " + msg);
        d->statusLbl->setStyleSheet("color:#cc4444; font-size:15px; font-weight:600;");
        d->runBtn->setEnabled(true);
    });
    connect(analysisMgr, &AnalysisManager::analysis_finished, this,
            [this](const QString& path, bool success) {
                d->runBtn->setEnabled(true);
                d->progressBar->setVisible(false);
                d->cameraProgressBar->setVisible(false);
                if (!d->jobIsMine) {
                    return;
                }
                // Only touch the status text if the user hasn't switched plugins
                // since starting this job — otherwise "Done."/"Failed" would land
                // on the now-different plugin's view. rebuild_session_list() still
                // runs regardless, so switching back later picks up the result.
                if (d->pluginCombo->currentData().toString() == d->jobPlugin) {
                    d->statusLbl->setText(success ? "Done." : "Failed — see log.");
                    d->statusLbl->setStyleSheet(
                        success ? "color:#44cc66; font-size:15px; font-weight:600;"
                                : "color:#cc4444; font-size:15px; font-weight:600;");
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
    d->sessionList->setItemDelegate(new SessionListDelegate(d->sessionList, d->sessionList));
    leftLay->addWidget(d->sessionList, 1);
    splitter->addWidget(leftPanel);

    connect(d->sessionList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || row >= d->sessions.size()) {
            return;
        }
        select_session(d->sessions[row].path);
    });

    // ── Right: run controls + results ───────────────────────────────────
    auto* rightPanel = new QWidget;
    auto* rightLay   = new QVBoxLayout(rightPanel);
    rightLay->setContentsMargins(0, 0, 0, 0);

    auto* runBox = new QGroupBox("Run analysis");
    auto* runLay = new QVBoxLayout(runBox);

    auto* controlsRow = new QHBoxLayout;
    d->pluginCombo    = new QComboBox;
    d->pluginCombo->addItem("Pose (YOLOv8)", "pose");
    d->pluginCombo->addItem("Face Masking (anonymize)", "face_mask");
    d->pluginCombo->addItem("Speaker Diarization", "diarize");
    d->pluginCombo->addItem("Facial Expression", "expression");
    d->pluginCombo->addItem("Multi-Camera Gaze Fusion", "gaze_fusion");
    d->pluginCombo->addItem("3D Pose Reconstruction", "pose3d");
    d->pluginCombo->addItem("EEG/Trigger ↔ Frame Sync", "trigger_sync");
    d->pluginCombo->addItem("Remote Heart Rate (rPPG, experimental)", "rppg");
    d->pluginCombo->addItem("2D Gaze (calibration-free)", "gaze2d");
    controlsRow->addWidget(new QLabel("Plugin:"));
    controlsRow->addWidget(d->pluginCombo);
    connect(d->pluginCombo, &QComboBox::currentIndexChanged, this, &AnalysisTabW::select_plugin);

    d->controlsStack = new QStackedWidget;

    // ── Pose controls page ──────────────────────────────────────────────
    auto* posePage     = new QWidget;
    auto* poseOuterLay = new QVBoxLayout(posePage);
    poseOuterLay->setContentsMargins(0, 0, 0, 0);
    auto* poseLay = new QHBoxLayout;
    poseOuterLay->addLayout(poseLay);

    d->modelCombo = new QComboBox;
    for (const auto& [label, value] : pose_model_options()) {
        d->modelCombo->addItem(label, value);
    }
    d->modelCombo->insertSeparator(d->modelCombo->count());
    for (const auto& [label, value] : pose_depth_model_options()) {
        d->modelCombo->addItem(label, value);
    }
    d->modelCombo->setToolTip(
        "Pose models produce a keypoint skeleton (overlay + metrics chart). "
        "Depth models produce a colorized per-pixel depth-map video instead — "
        "a completely different output, shown as plain video playback.");
    connect(d->modelCombo, &QComboBox::currentIndexChanged, this,
            &AnalysisTabW::on_pose_model_changed);
    poseLay->addWidget(new QLabel("Model:"));
    poseLay->addWidget(d->modelCombo, 1);

    d->skipSpin = new QSpinBox;
    d->skipSpin->setRange(1, 30);
    d->skipSpin->setValue(1);
    d->skipSpin->setPrefix("skip ");
    d->skipSpin->setToolTip(
        "Runs pose detection on every Nth frame instead of every frame. "
        "Skipped frames get no data at all (not interpolated) — the video "
        "overlay and chart fall back to the nearest analyzed frame for "
        "them. Higher values analyze long recordings faster at the cost "
        "of temporal resolution. Keep at 1 for the most complete result.");
    poseLay->addWidget(d->skipSpin);

    // Depth models are a completely separate output shape (a colorized
    // depth video, no keypoints/kinematics at all — see is_depth_model()'s
    // own doc comment). The model combo's tooltip already explains this,
    // but a tooltip is easy to miss; this persistent hint makes the
    // limitation, and today's workaround, discoverable without hovering.
    d->depthModeHintLbl = new QLabel(
        "Depth models produce a colorized depth video only — no pose "
        "keypoints/kinematics. Run again with a regular model (e.g. "
        "YOLOv8n-pose) to also get pose data for this session; both "
        "outputs are kept in separate pose/ and depth/ folders.");
    d->depthModeHintLbl->setWordWrap(true);
    d->depthModeHintLbl->setStyleSheet("color:#7070a0; font-size:11px; font-style:italic;");
    d->depthModeHintLbl->setVisible(false); // shown only when a depth model is selected
    poseOuterLay->addWidget(d->depthModeHintLbl);

    d->controlsStack->addWidget(posePage);

    // ── Face-mask controls page ─────────────────────────────────────────
    auto* faceMaskPage = new QWidget;
    auto* faceMaskLay  = new QHBoxLayout(faceMaskPage);
    faceMaskLay->setContentsMargins(0, 0, 0, 0);

    d->backendCombo = new QComboBox;
    d->backendCombo->addItem("MediaPipe", "mediapipe");
    d->backendCombo->setItemData(
        0, "Best recall, multiple faces. Downloads its model on first use.", Qt::ToolTipRole);
    d->backendCombo->addItem("YOLOv8-face", "yolov8");
    d->backendCombo->setItemData(
        1, "Community-maintained checkpoint — not officially hosted by Ultralytics.",
        Qt::ToolTipRole);
    d->backendCombo->addItem("OpenCV DNN (Recommended)", "opencv");
    d->backendCombo->setItemData(
        2, "No extra ML framework, but weaker recall on extreme head angles.", Qt::ToolTipRole);
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
    d->whisperModelCombo->addItem("tiny", "tiny");
    d->whisperModelCombo->addItem("base", "base");
    d->whisperModelCombo->addItem("small", "small");
    d->whisperModelCombo->addItem("medium", "medium");
    d->whisperModelCombo->addItem("large-v3", "large-v3");
    d->whisperModelCombo->setCurrentIndex(2); // small — good speed/accuracy default
    d->whisperModelCombo->setToolTip(
        "faster-whisper model size. Larger models are more accurate but slower "
        "and download more on first use.");
    diarizeLay->addWidget(new QLabel("Model:"));
    diarizeLay->addWidget(d->whisperModelCombo);

    d->languageCombo = new QComboBox;
    d->languageCombo->addItem("Auto-detect", "");
    d->languageCombo->addItem("English", "en");
    d->languageCombo->addItem("French", "fr");
    d->languageCombo->addItem("German", "de");
    d->languageCombo->addItem("Spanish", "es");
    d->languageCombo->addItem("Italian", "it");
    d->languageCombo->addItem("Portuguese", "pt");
    d->languageCombo->addItem("Dutch", "nl");
    d->languageCombo->addItem("Chinese", "zh");
    d->languageCombo->addItem("Japanese", "ja");
    diarizeLay->addWidget(new QLabel("Language:"));
    diarizeLay->addWidget(d->languageCombo);

    d->hfTokenEdit = new QLineEdit;
    d->hfTokenEdit->setEchoMode(QLineEdit::Password);
    d->hfTokenEdit->setPlaceholderText("Hugging Face token (optional — enables speaker labels)");
    d->hfTokenEdit->setText(d->settings.analysis.hfToken);
    d->hfTokenEdit->setToolTip(
        "Required only for speaker diarization (transcription always works without "
        "it). Create a free account at huggingface.co, accept the terms of use for "
        "pyannote/speaker-diarization-community-1, then generate a token at "
        "huggingface.co/settings/tokens. Saved to this profile's settings so you "
        "don't need to re-enter it next time.");
    connect(d->hfTokenEdit, &QLineEdit::textChanged, this,
            [this](const QString& text) { d->settings.analysis.hfToken = text; });
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

    // ── Facial Expression controls page ─────────────────────────────────
    auto* expressionPage = new QWidget;
    auto* expressionLay  = new QHBoxLayout(expressionPage);
    expressionLay->setContentsMargins(0, 0, 0, 0);

    d->expressionBackendCombo = new QComboBox;
    d->expressionBackendCombo->addItem("Heuristic (blendshapes)", "heuristic");
    d->expressionBackendCombo->setItemData(
        0,
        "Transparent, fast rule-based classifier over MediaPipe's blendshape scores — "
        "zero extra download, not a validated classifier.",
        Qt::ToolTipRole);
    d->expressionBackendCombo->addItem("FER+ (pretrained CNN)", "ferplus");
    d->expressionBackendCombo->setItemData(
        1,
        "Microsoft's FER+ ONNX model (MIT-licensed) — more validated, adds a 'Contempt' "
        "category, downloads an extra ~34MB model on first use.",
        Qt::ToolTipRole);
    d->expressionBackendCombo->addItem("py-feat (Action Units + emotion)", "pyfeat");
    d->expressionBackendCombo->setItemData(
        2,
        "py-feat's Detectorv1 — 20 FACS Action Units plus a 7-class emotion score, the most "
        "detailed of the 3 backends. Uses only the non-commercially-restricted default AU/"
        "emotion models (the alternate Detectorv2 model is research-only and not used here). "
        "Noticeably slower than the other backends — roughly 0.1-0.8s per frame on CPU, so "
        "expect longer analysis runs.",
        Qt::ToolTipRole);
    expressionLay->addWidget(new QLabel("Backend:"));
    expressionLay->addWidget(d->expressionBackendCombo);

    d->maxFacesSpin = new QSpinBox;
    d->maxFacesSpin->setRange(1, 10);
    d->maxFacesSpin->setValue(5);
    d->maxFacesSpin->setPrefix("max faces ");
    expressionLay->addWidget(d->maxFacesSpin);

    d->minConfidenceSpin = new QDoubleSpinBox;
    d->minConfidenceSpin->setRange(0.1, 1.0);
    d->minConfidenceSpin->setSingleStep(0.05);
    d->minConfidenceSpin->setValue(0.5);
    d->minConfidenceSpin->setPrefix("min conf ");
    expressionLay->addWidget(d->minConfidenceSpin);

    d->exprSkipSpin = new QSpinBox;
    d->exprSkipSpin->setRange(1, 30);
    d->exprSkipSpin->setValue(1);
    d->exprSkipSpin->setPrefix("skip ");
    expressionLay->addWidget(d->exprSkipSpin);
    d->controlsStack->addWidget(expressionPage);

    // ── Multi-Camera Gaze Fusion controls page ──────────────────────────
    // No plane-editing controls here — the plane is defined once, at Room
    // (Extrinsics) calibration time, in the same room frame as the
    // extrinsics; duplicating that UI here would risk two out-of-sync
    // plane definitions.
    auto* gazeFusionPage   = new QWidget;
    auto* gazeFusionCtlLay = new QHBoxLayout(gazeFusionPage);
    gazeFusionCtlLay->setContentsMargins(0, 0, 0, 0);

    d->minCamerasSpin = new QSpinBox;
    d->minCamerasSpin->setRange(1, 6);
    d->minCamerasSpin->setValue(2);
    d->minCamerasSpin->setPrefix("min cams ");
    d->minCamerasSpin->setToolTip(
        "Minimum simultaneous cameras required to compute a target point on the room "
        "plane. Rays from fewer cameras are still recorded, just without a target point.");
    gazeFusionCtlLay->addWidget(d->minCamerasSpin);

    d->gazeMinConfidenceSpin = new QDoubleSpinBox;
    d->gazeMinConfidenceSpin->setRange(0.1, 1.0);
    d->gazeMinConfidenceSpin->setSingleStep(0.05);
    d->gazeMinConfidenceSpin->setValue(0.5);
    d->gazeMinConfidenceSpin->setPrefix("min conf ");
    gazeFusionCtlLay->addWidget(d->gazeMinConfidenceSpin);

    d->gazeSkipSpin = new QSpinBox;
    d->gazeSkipSpin->setRange(1, 30);
    d->gazeSkipSpin->setValue(1);
    d->gazeSkipSpin->setPrefix("skip ");
    gazeFusionCtlLay->addWidget(d->gazeSkipSpin);
    d->controlsStack->addWidget(gazeFusionPage);

    // ── 3D Pose Reconstruction controls page ────────────────────────────
    // Reads the Pose plugin's already-computed .pose.json sidecars (see
    // run_analysis()'s pre-flight check) rather than running any new
    // inference — no model/backend choice needed here.
    auto* pose3dPage   = new QWidget;
    auto* pose3dCtlLay = new QHBoxLayout(pose3dPage);
    pose3dCtlLay->setContentsMargins(0, 0, 0, 0);

    d->pose3dMinCamerasSpin = new QSpinBox;
    d->pose3dMinCamerasSpin->setRange(2, 6);
    d->pose3dMinCamerasSpin->setValue(2);
    d->pose3dMinCamerasSpin->setPrefix("min cams ");
    d->pose3dMinCamerasSpin->setToolTip(
        "Minimum cameras a person cluster must span to be reconstructed at all "
        "(2 is the mathematical minimum for triangulation).");
    pose3dCtlLay->addWidget(d->pose3dMinCamerasSpin);

    d->maxReprojectionErrorSpin = new QDoubleSpinBox;
    d->maxReprojectionErrorSpin->setRange(1.0, 100.0);
    d->maxReprojectionErrorSpin->setValue(15.0);
    d->maxReprojectionErrorSpin->setSuffix(" px");
    d->maxReprojectionErrorSpin->setToolTip(
        "Per-view reprojection-error threshold for outlier-camera rejection during "
        "keypoint triangulation. A view further than this from the triangulated "
        "point is dropped and the point is re-triangulated from the rest.");
    pose3dCtlLay->addWidget(d->maxReprojectionErrorSpin);

    d->pose3dSkipSpin = new QSpinBox;
    d->pose3dSkipSpin->setRange(1, 30);
    d->pose3dSkipSpin->setValue(1);
    d->pose3dSkipSpin->setPrefix("skip ");
    pose3dCtlLay->addWidget(d->pose3dSkipSpin);

    d->pose3dSmoothingWindowSpin = new QSpinBox;
    d->pose3dSmoothingWindowSpin->setRange(1, 15);
    d->pose3dSmoothingWindowSpin->setSingleStep(2);
    d->pose3dSmoothingWindowSpin->setValue(1);
    d->pose3dSmoothingWindowSpin->setPrefix("smooth ");
    d->pose3dSmoothingWindowSpin->setToolTip(
        "Centered per-track median filter width (in valid ticks), for the room view's "
        "optional smoothed display only — 1 = off. The raw triangulated positions are "
        "always kept too; see the \"Show smoothed\" checkbox in the results view.");
    pose3dCtlLay->addWidget(d->pose3dSmoothingWindowSpin);
    d->controlsStack->addWidget(pose3dPage);

    // ── EEG/Trigger ↔ Frame Sync controls page ──────────────────────────
    // No model/backend/skip controls — this plugin has nothing to tune, it's
    // a deterministic nearest-timestamp lookup between trigger.csv and every
    // camera's timestamps_camN.csv, computed synchronously in C++.
    auto* triggerSyncPage   = new QWidget;
    auto* triggerSyncCtlLay = new QHBoxLayout(triggerSyncPage);
    triggerSyncCtlLay->setContentsMargins(0, 0, 0, 0);
    auto* triggerSyncHint = new QLabel(
        "Resolves every event in this session's trigger.csv (e.g. an EEG "
        "amplifier's parallel-port trigger cable) to its nearest frame in "
        "every camera. Nothing to configure — click Run.");
    triggerSyncHint->setWordWrap(true);
    triggerSyncHint->setProperty("role", "muted");
    triggerSyncCtlLay->addWidget(triggerSyncHint, 1);
    d->controlsStack->addWidget(triggerSyncPage);

    // ── Remote Heart Rate (rPPG) controls page ──────────────────────────
    // No frame-skip control, unlike every sibling plugin above — a real
    // Nyquist-sampling reason, not an oversight: skipping frames would
    // downsample the pulse signal itself, and this plugin already needs
    // near-every-frame sampling to resolve a 0.7-3.0 Hz physiological
    // band. See analysis/run_rppg.py's own module doc comment.
    auto* rppgPage   = new QWidget;
    auto* rppgCtlLay = new QHBoxLayout(rppgPage);
    rppgCtlLay->setContentsMargins(0, 0, 0, 0);

    d->rppgBackendCombo = new QComboBox;
    d->rppgBackendCombo->addItem("POS (recommended)", "pos");
    d->rppgBackendCombo->addItem("CHROM", "chrom");
    d->rppgBackendCombo->addItem("Green (naive baseline)", "green");
    d->rppgBackendCombo->setItemData(
        0,
        "Plane-Orthogonal-to-Skin (Wang et al. 2017) — generally the most robust "
        "classical rPPG method.",
        Qt::ToolTipRole);
    d->rppgBackendCombo->setItemData(
        1, "Chrominance-based (de Haan & Jeanne 2013) — a solid, simpler alternative to POS.",
        Qt::ToolTipRole);
    d->rppgBackendCombo->setItemData(
        2,
        "Raw green-channel signal, no motion/illumination compensation at all — "
        "fastest, but the most sensitive to any movement. Kept for comparison/debugging.",
        Qt::ToolTipRole);
    connect(d->rppgBackendCombo, &QComboBox::currentIndexChanged, this,
            &AnalysisTabW::reload_current_camera_result);
    rppgCtlLay->addWidget(d->rppgBackendCombo);

    d->rppgWindowSecSpin = new QDoubleSpinBox;
    d->rppgWindowSecSpin->setRange(4.0, 30.0);
    d->rppgWindowSecSpin->setSingleStep(1.0);
    d->rppgWindowSecSpin->setValue(10.0);
    d->rppgWindowSecSpin->setSuffix(" s window");
    d->rppgWindowSecSpin->setToolTip(
        "HR-analysis window length. Longer windows give finer frequency resolution "
        "(more precise BPM) but respond more slowly to real heart-rate changes.");
    rppgCtlLay->addWidget(d->rppgWindowSecSpin);

    d->rppgHopSecSpin = new QDoubleSpinBox;
    d->rppgHopSecSpin->setRange(0.5, 10.0);
    d->rppgHopSecSpin->setSingleStep(0.5);
    d->rppgHopSecSpin->setValue(2.0);
    d->rppgHopSecSpin->setSuffix(" s hop");
    rppgCtlLay->addWidget(d->rppgHopSecSpin);

    d->rppgSmoothingSpin = new QSpinBox;
    d->rppgSmoothingSpin->setRange(1, 15);
    d->rppgSmoothingSpin->setSingleStep(2);
    d->rppgSmoothingSpin->setValue(1);
    d->rppgSmoothingSpin->setPrefix("smooth ");
    d->rppgSmoothingSpin->setToolTip(
        "Centered median-filter width (in windows) applied when writing the "
        "smoothed_bpm series. 1 = no smoothing (default) — raw bpm is always kept "
        "too, regardless of this setting.");
    rppgCtlLay->addWidget(d->rppgSmoothingSpin);
    d->controlsStack->addWidget(rppgPage);

    // ── 2D Gaze (calibration-free) controls page ────────────────────────
    // No backend choice (unlike rPPG's 3 algorithms) — there's only one
    // iris-offset heuristic, the same one already running live in the
    // Real-time tab. Frame-skip IS offered here (unlike rPPG), matching
    // Pose's/Expression's convention — see analysis/run_gaze2d.py's own
    // module doc comment for why this differs from rPPG's Nyquist-driven
    // omission.
    auto* gaze2dPage   = new QWidget;
    auto* gaze2dCtlLay = new QHBoxLayout(gaze2dPage);
    gaze2dCtlLay->setContentsMargins(0, 0, 0, 0);

    d->gaze2dMinConfidenceSpin = new QDoubleSpinBox;
    d->gaze2dMinConfidenceSpin->setRange(0.1, 1.0);
    d->gaze2dMinConfidenceSpin->setSingleStep(0.05);
    d->gaze2dMinConfidenceSpin->setValue(0.5);
    d->gaze2dMinConfidenceSpin->setPrefix("min conf ");
    gaze2dCtlLay->addWidget(d->gaze2dMinConfidenceSpin);

    d->gaze2dSkipSpin = new QSpinBox;
    d->gaze2dSkipSpin->setRange(1, 30);
    d->gaze2dSkipSpin->setValue(1);
    d->gaze2dSkipSpin->setPrefix("skip ");
    gaze2dCtlLay->addWidget(d->gaze2dSkipSpin);
    d->controlsStack->addWidget(gaze2dPage);

    controlsRow->addWidget(d->controlsStack, 1);

    d->runBtn = new QPushButton("▶  Run");
    d->runBtn->setFixedHeight(34);
    d->runBtn->setCursor(Qt::PointingHandCursor);
    // Same green "go" convention as AdminPanelDialog::launchBtn (also a
    // "▶ …" primary action) — a glossy gradient here specifically per the
    // user's ask for something more colorful than the plain default
    // QPushButton style every other button in this tab still uses.
    d->runBtn->setStyleSheet(
        "QPushButton { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
        "    stop:0 #33bb66, stop:1 #229944);"
        "  border: 1px solid #33aa55; border-radius: 6px;"
        "  padding: 6px 22px; color: #ffffff; font-size: 13px; font-weight: bold; }"
        "QPushButton:hover { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
        "    stop:0 #3fd479, stop:1 #28b552); border-color: #55ee88; }"
        "QPushButton:pressed { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
        "    stop:0 #1f8a44, stop:1 #186e35); }"
        "QPushButton:disabled { background: #12251a; border-color: #1e3a28; color: #4a6a55; }");
    connect(d->runBtn, &QPushButton::clicked, this, &AnalysisTabW::run_analysis);
    controlsRow->addWidget(d->runBtn);
    runLay->addLayout(controlsRow);

    d->statusLbl = new QLabel("Select a session to begin.");
    d->statusLbl->setStyleSheet("color:#6060a0; font-size:15px; font-weight:600;");
    runLay->addWidget(d->statusLbl);

    // Coarse, session-wide sibling of progressBar below — which camera out
    // of N is currently being processed (Pose plugin's multi-camera
    // process_session() runs only; stays hidden and inert for every other
    // plugin, since only run_pose.py prints the "Camera N/M:" line this
    // parses). Distinct accent color (blue vs. progressBar's green) so the
    // two are visually distinguishable when both are visible at once.
    d->cameraProgressBar = new QProgressBar;
    d->cameraProgressBar->setRange(0, 1);
    d->cameraProgressBar->setTextVisible(true);
    d->cameraProgressBar->setFormat("Camera");
    d->cameraProgressBar->setFixedHeight(22);
    d->cameraProgressBar->setVisible(false);
    d->cameraProgressBar->setStyleSheet(
        "QProgressBar { background:#0a0a1a; border:1px solid #2a3a6a; border-radius:5px;"
        " text-align:center; color:#dde4ff; font-size:12px; font-weight:600; }"
        "QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        "    stop:0 #2f5cc0, stop:1 #4d7fe0); border-radius:4px; }");
    runLay->addWidget(d->cameraProgressBar);

    // tqdm-style progress display for the per-frame ticker every plugin
    // script prints ("  42.7%  (1200/2810)  18.3s elapsed") — parsed out of
    // output_received() below instead of scrolling by as tiny log text.
    // Hidden outside an active run.
    d->progressBar = new QProgressBar;
    d->progressBar->setRange(0, 1000); // per-mille, for one-decimal percent resolution
    d->progressBar->setTextVisible(true);
    d->progressBar->setFormat("%p%");
    d->progressBar->setFixedHeight(22);
    d->progressBar->setVisible(false);
    d->progressBar->setStyleSheet(
        "QProgressBar { background:#0a0a1a; border:1px solid #1e3a28; border-radius:5px;"
        " text-align:center; color:#dde4ff; font-size:12px; font-weight:600; }"
        "QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        "    stop:0 #229944, stop:1 #44aa66); border-radius:4px; }");
    runLay->addWidget(d->progressBar);

    d->logView = new QTextEdit;
    d->logView->setReadOnly(true);
    d->logView->setMaximumHeight(110);
    d->logView->setStyleSheet(
        "QTextEdit { background:#060810; color:#55cc7a; font-family:'Consolas','Courier "
        "New',monospace;"
        " font-size:13px; border:1px solid #1e3a28; border-radius:6px; padding:8px; }");
    runLay->addWidget(d->logView);

    rightLay->addWidget(runBox);

    // ── Results: camera/keypoint pickers + player + chart ──────────────
    // sourceRowW/micRowW are mutually exclusive containers (see
    // Impl::sourceRowW doc comment) so select_plugin() can show exactly one.
    d->sourceRowW    = new QWidget;
    auto* resultsRow = new QHBoxLayout(d->sourceRowW);
    resultsRow->setContentsMargins(0, 0, 0, 0);
    d->cameraCombo = new QComboBox;
    resultsRow->addWidget(new QLabel("Camera:"));
    resultsRow->addWidget(d->cameraCombo);
    connect(d->cameraCombo, &QComboBox::currentIndexChanged, this, &AnalysisTabW::select_camera);

    // Pairs a label with its combo in one container so select_plugin() can
    // hide both together with a single setVisible() call — a bare
    // resultsRow->addWidget(new QLabel(...)) has no stored pointer, so
    // without this the label would stay visible even once its combo (the
    // only thing select_plugin() could otherwise toggle) is hidden.
    auto make_field = [&](const QString& labelText, QComboBox* combo) {
        auto* field = new QWidget;
        auto* lay   = new QHBoxLayout(field);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addWidget(new QLabel(labelText));
        lay->addWidget(combo, 1);
        resultsRow->addWidget(field, 1);
        return field;
    };

    d->keypointCombo  = new QComboBox;
    d->keypointFieldW = make_field("Keypoint:", d->keypointCombo);
    connect(d->keypointCombo, &QComboBox::currentIndexChanged, this,
            &AnalysisTabW::update_kinematics_chart);

    d->blendshapeCombo  = new QComboBox;
    d->blendshapeFieldW = make_field("Blendshape:", d->blendshapeCombo);
    d->blendshapeFieldW->setVisible(false); // shown only for the expression plugin
    connect(d->blendshapeCombo, &QComboBox::currentIndexChanged, this,
            &AnalysisTabW::update_expression_view);

    d->trackCombo = new QComboBox;
    d->trackCombo->setToolTip(
        "Filters the stats/CSV export to one reconstructed person. The 3D room "
        "view always shows every tracked person regardless of this selection.");
    d->trackFieldW = make_field("Track:", d->trackCombo);
    d->trackFieldW->setVisible(false); // shown only for the pose3d plugin
    connect(d->trackCombo, &QComboBox::currentIndexChanged, this,
            &AnalysisTabW::update_pose3d_view);

    d->openFolderBtn = new QPushButton("Open output folder");
    d->openFolderBtn->setVisible(false);
    d->openFolderBtn->setEnabled(false);
    connect(d->openFolderBtn, &QPushButton::clicked, this, &AnalysisTabW::open_output_folder);
    resultsRow->addWidget(d->openFolderBtn);

    rightLay->addWidget(d->sourceRowW);

    d->micRowW   = new QWidget;
    auto* micRow = new QHBoxLayout(d->micRowW);
    micRow->setContentsMargins(0, 0, 0, 0);
    d->micCombo = new QComboBox;
    micRow->addWidget(new QLabel("Mic:"));
    micRow->addWidget(d->micCombo);
    connect(d->micCombo, &QComboBox::currentIndexChanged, this, &AnalysisTabW::select_camera);

    d->transcriptStatsLbl = new QLabel;
    d->transcriptStatsLbl->setStyleSheet("color:#8888b8; font-size:13px; font-weight:600;");
    micRow->addWidget(d->transcriptStatsLbl, 1);

    d->exportTranscriptBtn = new QPushButton("Export CSV");
    d->exportTranscriptBtn->setToolTip("Exports timestamp/speaker/text for every segment as CSV.");
    connect(d->exportTranscriptBtn, &QPushButton::clicked, this,
            &AnalysisTabW::export_transcript_csv);
    micRow->addWidget(d->exportTranscriptBtn);

    d->micRowW->setVisible(false); // shown only for the diarize plugin
    rightLay->addWidget(d->micRowW);

    // Whole-clip waveform, same bipolar-envelope visual style as the Audio
    // settings tab's live monitor — static mode (see AudioWaveformW::
    // set_static_envelope()) with a playhead synced to d->player instead of
    // a live rolling history.
    d->diarizeWaveform = new AudioWaveformW;
    d->diarizeWaveform->setMinimumHeight(100); // sizeHint()'s per-channel formula assumes the
                                               // Audio tab's multi-mic monitor; one prominent
                                               // trace here reads better taller.
    d->diarizeWaveform->setVisible(false);     // shown only for the diarize plugin
    rightLay->addWidget(d->diarizeWaveform);
    d->diarizeWaveform->set_seek_callback([this](qint64 ms) { d->player->seek(ms); });

    // Speaker-legend row: one color swatch + label per speaker found in the
    // current session's transcript, rebuilt by rebuild_speaker_legend()
    // every time reload_current_camera_result() loads a new diarize result.
    d->speakerLegendRowW   = new QWidget;
    auto* speakerLegendLay = new QHBoxLayout(d->speakerLegendRowW);
    speakerLegendLay->setContentsMargins(0, 4, 0, 0);
    d->speakerLegendRowW->setVisible(false); // shown only for the diarize plugin
    rightLay->addWidget(d->speakerLegendRowW);

    // ── Subject picker: one checkable chip per detected subject, so the
    //    chart/overlay can show several subjects' traces at once instead of
    //    always just subject 0. Pose only — own-container row, mirroring
    //    kinematicsRowW's convention, rebuilt by rebuild_subject_chips()
    //    whenever a new session/camera loads.
    d->subjectPickerRowW   = new QWidget;
    auto* subjectPickerLay = new QHBoxLayout(d->subjectPickerRowW);
    subjectPickerLay->setContentsMargins(0, 0, 0, 4);
    subjectPickerLay->setSpacing(6);
    d->subjectPickerRowW->setVisible(false); // shown only for the pose plugin, and only
                                             // when the session has >1 detected subject
    rightLay->addWidget(d->subjectPickerRowW);

    // ── Kinematics view controls: reshape how the current keypoint's data
    //    is plotted (Position/Speed/Acceleration), not what gets launched.
    //    Pose only — wrapped in a container widget so select_plugin() can
    //    hide the whole row for the Face Masking plugin with one call.
    d->kinematicsRowW   = new QWidget;
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
        "Subject identity is not tracked across frames — each Subject number "
        "above is per-frame detection order, not a tracked individual (safe "
        "to treat as one continuous subject only within a single, unbroken "
        "detection run).");
    kinematicsRow->addWidget(d->kinematicsStatsLbl, 1);

    d->exportKinematicsBtn = new QPushButton("Export CSV");
    d->exportKinematicsBtn->setToolTip(
        "Exports timestamp/position/speed/acceleration for EVERY keypoint "
        "(not just the one currently shown in the chart), every "
        "currently-selected subject, and every camera analyzed with the "
        "currently-selected model — into one CSV under this session's "
        "kinematics/ folder, in raw pixel units, ignoring the Scale "
        "spinbox above.");
    connect(d->exportKinematicsBtn, &QPushButton::clicked, this,
            &AnalysisTabW::export_kinematics_csv);
    kinematicsRow->addWidget(d->exportKinematicsBtn);

    rightLay->addWidget(d->kinematicsRowW);

    // ── Expression view controls: %-per-category breakdown + CSV export.
    //    Expression only — own container so select_plugin() can hide the
    //    whole row with one call, mirroring kinematicsRowW.
    d->expressionRowW   = new QWidget;
    auto* expressionRow = new QHBoxLayout(d->expressionRowW);
    expressionRow->setContentsMargins(0, 0, 0, 0);

    d->expressionStatsLbl = new QLabel;
    d->expressionStatsLbl->setStyleSheet("color:#7070a0; font-size:11px;");
    d->expressionStatsLbl->setToolTip(
        "Subject identity is not tracked across frames — treat subject 0 as one "
        "continuous face only for single-face sessions. Percentages are of analyzed "
        "frames, not wall-clock time (--skip means frames aren't evenly time-spaced).");
    expressionRow->addWidget(d->expressionStatsLbl, 1);

    d->exportExpressionBtn = new QPushButton("Export CSV");
    d->exportExpressionBtn->setToolTip(
        "Exports timestamp/bbox/dominant-expression/blendshape-scores for every "
        "detected face as CSV.");
    connect(d->exportExpressionBtn, &QPushButton::clicked, this,
            &AnalysisTabW::export_expression_csv);
    expressionRow->addWidget(d->exportExpressionBtn);

    d->expressionRowW->setVisible(false); // shown only for the expression plugin
    rightLay->addWidget(d->expressionRowW);

    // ── Gaze-fusion view controls: fit-quality stats + CSV export. Gaze
    //    Fusion only — own container so select_plugin() can hide the whole
    //    row with one call, mirroring expressionRowW/kinematicsRowW.
    d->gazeFusionRowW   = new QWidget;
    auto* gazeFusionRow = new QHBoxLayout(d->gazeFusionRowW);
    gazeFusionRow->setContentsMargins(0, 0, 0, 0);

    d->gazeStatsLbl = new QLabel;
    d->gazeStatsLbl->setStyleSheet("color:#7070a0; font-size:11px;");
    d->gazeStatsLbl->setToolTip(
        "Subject identity is not tracked across frames — treat subject 0 as one "
        "continuous face only for single-face sessions.");
    gazeFusionRow->addWidget(d->gazeStatsLbl, 1);

    d->exportGazeBtn = new QPushButton("Export CSV");
    d->exportGazeBtn->setToolTip(
        "Exports tick/timestamp/fused-ray/residual/target for every fused frame as CSV.");
    connect(d->exportGazeBtn, &QPushButton::clicked, this, &AnalysisTabW::export_gaze_csv);
    gazeFusionRow->addWidget(d->exportGazeBtn);

    d->gazeFusionRowW->setVisible(false); // shown only for the gaze_fusion plugin
    rightLay->addWidget(d->gazeFusionRowW);

    // ── 3D Pose Reconstruction view controls: reconstruction-quality stats
    //    + CSV export. Pose3D only — own container so select_plugin() can
    //    hide the whole row with one call, mirroring gazeFusionRowW.
    d->pose3dRowW   = new QWidget;
    auto* pose3dRow = new QHBoxLayout(d->pose3dRowW);
    pose3dRow->setContentsMargins(0, 0, 0, 0);

    d->pose3dStatsLbl = new QLabel;
    d->pose3dStatsLbl->setStyleSheet("color:#7070a0; font-size:11px;");
    pose3dRow->addWidget(d->pose3dStatsLbl, 1);

    // Scoped to the 3D room view only, not the 2D video overlay
    // (PoseOverlayPlayerW::set_skeleton3d_result()) — the overlay's
    // reprojected_px values are precomputed in Python from the RAW
    // triangulated point specifically so it shows ground truth against
    // real video pixels; reprojecting the smoothed point would need a
    // second Python-side reprojection pass, a deliberate future extension,
    // not built here.
    d->showSmoothedCheck = new QCheckBox("Show smoothed");
    d->showSmoothedCheck->setChecked(false);
    d->showSmoothedCheck->setToolTip(
        "Room view only: draw the centered-median-smoothed trajectory (set via the "
        "\"smooth\" control above, before running) instead of the raw triangulated "
        "positions. The 2D video overlay always shows raw positions, regardless of "
        "this toggle.");
    connect(d->showSmoothedCheck, &QCheckBox::toggled, this,
            [this](bool checked) { d->skeleton3dRoomView->set_show_smoothed(checked); });
    pose3dRow->addWidget(d->showSmoothedCheck);

    d->exportPose3dBtn = new QPushButton("Export CSV");
    d->exportPose3dBtn->setToolTip(
        "Exports tick/timestamp/track/keypoint/raw position/smoothed position/validity/"
        "reprojection-error for every reconstructed keypoint as CSV (long format), "
        "filtered by the Track picker above (or every track if \"All\").");
    connect(d->exportPose3dBtn, &QPushButton::clicked, this, &AnalysisTabW::export_skeleton3d_csv);
    pose3dRow->addWidget(d->exportPose3dBtn);

    d->pose3dRowW->setVisible(false); // shown only for the pose3d plugin
    rightLay->addWidget(d->pose3dRowW);

    // ── Dyad Analysis row: interpersonal distance/approach-rate/facingness/
    //    congruent-motion between two selected tracks, computed live from
    //    the already-loaded currentSkeleton3D (compute_dyadic_kinematics(),
    //    src/analysis/dyadic_kinematics.hpp) — no new Python job. Pose3d
    //    only, own container so select_plugin() can hide the whole row with
    //    one call, matching pose3dRowW's own convention. Track A/B combos
    //    are (re-)populated in reload_current_camera_result()'s pose3d
    //    branch, reusing the same trackIds list that already builds
    //    trackCombo, right below where this row is inserted.
    d->dyadRowW   = new QWidget;
    auto* dyadRow = new QHBoxLayout(d->dyadRowW);
    dyadRow->setContentsMargins(0, 0, 0, 0);

    dyadRow->addWidget(new QLabel("Dyad:"));
    d->dyadTrackACombo = new QComboBox;
    d->dyadTrackACombo->setToolTip("First person of the pair to compare.");
    dyadRow->addWidget(d->dyadTrackACombo);
    dyadRow->addWidget(new QLabel("↔")); // <-> (left-right arrow), the "compared to" glyph
    d->dyadTrackBCombo = new QComboBox;
    d->dyadTrackBCombo->setToolTip("Second person of the pair to compare.");
    dyadRow->addWidget(d->dyadTrackBCombo);

    d->dyadMetricCombo = new QComboBox;
    d->dyadMetricCombo->addItem("Distance (mm)", "distance");
    d->dyadMetricCombo->addItem("Approach rate (mm/s)", "approach_rate");
    d->dyadMetricCombo->addItem("Facingness (cosine)", "facing");
    d->dyadMetricCombo->addItem("Congruent motion (r)", "congruent_motion");
    d->dyadMetricCombo->setToolTip(
        "Facingness: torso-heading cosine similarity. -1 = oriented oppositely (commonly "
        "face-to-face in a two-person interaction, but also consistent with standing "
        "back-to-back facing directly away from each other — position-independent by "
        "construction). +1 = oriented the same way (e.g. side-by-side). 0 = perpendicular.\n\n"
        "Congruent motion: rolling-window Pearson correlation of the two people's own "
        "instantaneous speed.");
    dyadRow->addWidget(d->dyadMetricCombo);

    d->dyadWindowSpin = new QSpinBox;
    d->dyadWindowSpin->setRange(5, 120);
    d->dyadWindowSpin->setValue(30);
    d->dyadWindowSpin->setPrefix("window ");
    d->dyadWindowSpin->setToolTip(
        "Trailing window size (in paired valid samples), only meaningful for the Congruent "
        "motion metric.");
    dyadRow->addWidget(d->dyadWindowSpin);

    d->dyadStatsLbl = new QLabel;
    d->dyadStatsLbl->setStyleSheet("color:#7070a0; font-size:11px;");
    dyadRow->addWidget(d->dyadStatsLbl, 1);

    d->exportDyadBtn = new QPushButton("Export CSV");
    d->exportDyadBtn->setToolTip(
        "Exports tick/timestamp/distance/approach-rate/facingness/congruent-motion for the "
        "selected pair as CSV.");
    connect(d->exportDyadBtn, &QPushButton::clicked, this, &AnalysisTabW::export_dyad_csv);
    dyadRow->addWidget(d->exportDyadBtn);

    for (auto* combo : {d->dyadTrackACombo, d->dyadTrackBCombo, d->dyadMetricCombo}) {
        connect(combo, &QComboBox::currentIndexChanged, this, &AnalysisTabW::update_dyadic_view);
    }
    connect(d->dyadWindowSpin, &QSpinBox::valueChanged, this, &AnalysisTabW::update_dyadic_view);

    d->dyadRowW->setVisible(false); // shown only for the pose3d plugin
    rightLay->addWidget(d->dyadRowW);

    // ── EEG/Trigger sync view controls: resolved-trigger count + CSV
    //    export. trigger_sync only — own container so select_plugin() can
    //    hide the whole row with one call, mirroring pose3dRowW.
    d->triggerSyncRowW   = new QWidget;
    auto* triggerSyncRow = new QHBoxLayout(d->triggerSyncRowW);
    triggerSyncRow->setContentsMargins(0, 0, 0, 0);

    d->triggerSyncStatsLbl = new QLabel;
    d->triggerSyncStatsLbl->setStyleSheet("color:#7070a0; font-size:11px;");
    triggerSyncRow->addWidget(d->triggerSyncStatsLbl, 1);

    d->exportTriggerSyncBtn = new QPushButton("Export CSV");
    d->exportTriggerSyncBtn->setToolTip(
        "Exports every trigger event with its resolved nearest frame_id/timing "
        "error per camera as CSV, for use with MNE-Python or another external "
        "EEG-analysis pipeline.");
    connect(d->exportTriggerSyncBtn, &QPushButton::clicked, this,
            &AnalysisTabW::export_trigger_sync_csv);
    triggerSyncRow->addWidget(d->exportTriggerSyncBtn);

    d->triggerSyncRowW->setVisible(false); // shown only for the trigger_sync plugin
    rightLay->addWidget(d->triggerSyncRowW);

    // ── Remote Heart Rate (rPPG) view controls: a persistent disclaimer
    //    banner (always visible while this plugin is selected — reuses the
    //    Real-time tab's pausedBanner amber visual language — never just a
    //    tooltip, since this plugin's whole safety framing depends on the
    //    caveat never being missable), a raw/smoothed toggle for the shared
    //    chart, stats readout, and CSV export. rppg only — own container so
    //    select_plugin() can hide the whole row with one call, mirroring
    //    triggerSyncRowW.
    d->rppgRowW      = new QWidget;
    auto* rppgRowLay = new QVBoxLayout(d->rppgRowW);
    rppgRowLay->setContentsMargins(0, 0, 0, 0);
    rppgRowLay->setSpacing(4);

    d->rppgDisclaimerLbl = new QLabel(
        "⚠ Experimental research estimate only — not a medical device, not clinically validated.");
    d->rppgDisclaimerLbl->setStyleSheet(
        "QLabel { color:#ddaa44; background:rgba(221,170,68,0.15); "
        "border:1px solid #ddaa44; border-radius:4px; padding:6px 10px; font-weight:600; }");
    d->rppgDisclaimerLbl->setWordWrap(true);
    rppgRowLay->addWidget(d->rppgDisclaimerLbl);

    auto* rppgStatsRow       = new QHBoxLayout;
    d->rppgShowSmoothedCheck = new QCheckBox("Show smoothed");
    d->rppgShowSmoothedCheck->setToolTip(
        "Toggles the chart between the raw per-window BPM series and a centered "
        "median-filtered version (width set by the \"smooth\" control above). "
        "CSV export always includes both columns regardless of this toggle.");
    connect(d->rppgShowSmoothedCheck, &QCheckBox::toggled, this, &AnalysisTabW::update_rppg_view);
    rppgStatsRow->addWidget(d->rppgShowSmoothedCheck);

    d->rppgQualityBadge = new QLabel("—");
    d->rppgQualityBadge->setStyleSheet(badge_stylesheet(RmsQuality::Poor));
    d->rppgQualityBadge->setToolTip(
        "Signal-quality tier for this result: mean pulse-SNR across usable windows, "
        "forced to Poor whenever fewer than 60% of windows had a reliable face-ROI "
        "detection (see rppg_quality_for()).");
    rppgStatsRow->addWidget(d->rppgQualityBadge);

    d->rppgStatsLbl = new QLabel;
    d->rppgStatsLbl->setStyleSheet("color:#7070a0; font-size:11px;");
    rppgStatsRow->addWidget(d->rppgStatsLbl, 1);

    d->exportRppgBtn = new QPushButton("Export CSV");
    d->exportRppgBtn->setToolTip(
        "Exports start/end time, raw bpm, smoothed bpm, SNR, and valid-frame-fraction "
        "for every analysis window as CSV.");
    connect(d->exportRppgBtn, &QPushButton::clicked, this, &AnalysisTabW::export_rppg_csv);
    rppgStatsRow->addWidget(d->exportRppgBtn);
    rppgRowLay->addLayout(rppgStatsRow);

    d->rppgRowW->setVisible(false); // shown only for the rppg plugin
    rightLay->addWidget(d->rppgRowW);

    // ── 2D Gaze (calibration-free) view controls: metric combo (dx / dy /
    //    magnitude), stats readout, and CSV export. gaze2d only — own
    //    container so select_plugin() can hide the whole row with one call.
    d->gaze2dRowW      = new QWidget;
    auto* gaze2dRowLay = new QHBoxLayout(d->gaze2dRowW);
    gaze2dRowLay->setContentsMargins(0, 0, 0, 0);

    d->gaze2dMetricCombo = new QComboBox;
    d->gaze2dMetricCombo->addItem("dx", "dx");
    d->gaze2dMetricCombo->addItem("dy", "dy");
    d->gaze2dMetricCombo->addItem("magnitude", "magnitude");
    connect(d->gaze2dMetricCombo, &QComboBox::currentIndexChanged, this,
            &AnalysisTabW::update_gaze2d_view);
    gaze2dRowLay->addWidget(new QLabel("Metric:"));
    gaze2dRowLay->addWidget(d->gaze2dMetricCombo);

    d->gaze2dStatsLbl = new QLabel;
    d->gaze2dStatsLbl->setStyleSheet("color:#7070a0; font-size:11px;");
    gaze2dRowLay->addWidget(d->gaze2dStatsLbl, 1);

    d->exportGaze2dBtn = new QPushButton("Export CSV");
    connect(d->exportGaze2dBtn, &QPushButton::clicked, this, &AnalysisTabW::export_gaze2d_csv);
    gaze2dRowLay->addWidget(d->exportGaze2dBtn);

    d->gaze2dRowW->setVisible(false); // shown only for the gaze2d plugin
    rightLay->addWidget(d->gaze2dRowW);

    d->resultsSplitter     = new QSplitter(Qt::Horizontal);
    auto*& resultsSplitter = d->resultsSplitter;
    d->player = new PoseOverlayPlayerW;    // reused for audio-only playback in diarize mode too —
    resultsSplitter->addWidget(d->player); // set_video() just calls QMediaPlayer::setSource(),
                                           // which plays a .wav fine with no video frames
                                           // arriving. Also reused for expression mode's
                                           // bbox+label overlay via set_expression_result()
                                           // (mutually exclusive with set_pose_result() — see
                                           // pose_overlay_player_w.hpp).

    d->chart = new MetricsChartW;
    resultsSplitter->addWidget(d->chart);

    d->transcriptTable = new QTableWidget(0, 4);
    d->transcriptTable->setHorizontalHeaderLabels({"Start", "End", "Speaker", "Text"});
    d->transcriptTable->horizontalHeader()->setStretchLastSection(true);
    d->transcriptTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    d->transcriptTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    d->transcriptTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    d->transcriptTable->setColumnWidth(0, 70);
    d->transcriptTable->setColumnWidth(1, 70);
    d->transcriptTable->setColumnWidth(2, 110);
    d->transcriptTable->setWordWrap(true);
    d->transcriptTable->setStyleSheet(
        "QTableWidget { font-size: 13px; } "
        "QHeaderView::section { font-size: 12px; font-weight: 600; color: #a0a0c8; }");
    d->transcriptTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    d->transcriptTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    d->transcriptTable->setSelectionMode(QAbstractItemView::SingleSelection);
    d->transcriptTable->setVisible(false); // shown only for the diarize plugin
    connect(d->transcriptTable, &QTableWidget::cellClicked, this, [this](int row, int) {
        auto* item = d->transcriptTable->item(row, 0);
        if (item) {
            d->player->seek(item->data(Qt::UserRole).toLongLong());
        }
    });
    resultsSplitter->addWidget(d->transcriptTable);

    d->roomView = new GazeRoomViewW; // shown only for the gaze_fusion plugin
    d->roomView->setVisible(false);
    resultsSplitter->addWidget(d->roomView);

    d->skeleton3dRoomView = new Skeleton3DRoomViewW; // shown only for the pose3d plugin
    d->skeleton3dRoomView->setVisible(false);
    resultsSplitter->addWidget(d->skeleton3dRoomView);

    d->triggerSyncTable = new QTableWidget(0, 7); // 7 fixed cols; camera cols added dynamically
    d->triggerSyncTable->setHorizontalHeaderLabels(
        {"Row", "Elapsed (ms)", "Wall clock", "Source", "Label", "Code", "Value"});
    d->triggerSyncTable->horizontalHeader()->setStretchLastSection(true);
    d->triggerSyncTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    d->triggerSyncTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    d->triggerSyncTable->setSelectionMode(QAbstractItemView::SingleSelection);
    d->triggerSyncTable->setVisible(false); // shown only for the trigger_sync plugin
    connect(d->triggerSyncTable, &QTableWidget::cellClicked, this, [this](int row, int) {
        if (row < 0 || row >= d->currentTriggerFrameMap.trigger_count()) {
            return;
        }
        const int camIdx = d->cameraCombo->currentIndex();
        const auto& hits = d->currentTriggerFrameMap.row(row).frames;
        if (camIdx < 0 || camIdx >= hits.size()) {
            return;
        }
        const int64_t posMs = hits[camIdx].videoPositionMs;
        if (posMs >= 0) {
            d->player->seek(posMs);
        }
    });
    resultsSplitter->addWidget(d->triggerSyncTable);

    resultsSplitter->setStretchFactor(0, 1);
    resultsSplitter->setStretchFactor(1, 1);
    resultsSplitter->setStretchFactor(2, 1);
    resultsSplitter->setStretchFactor(3, 1);
    resultsSplitter->setStretchFactor(4, 1);
    resultsSplitter->setStretchFactor(5, 1);
    rightLay->addWidget(resultsSplitter, 1);

    d->chart->set_seek_callback([this](int64_t ms) { d->player->seek(ms); });
    connect(d->player, &PoseOverlayPlayerW::position_changed, this, [this](int64_t ms) {
        d->chart->set_playhead_ms(ms);
        highlight_active_transcript_row(ms);
        if (is_gaze_fusion_plugin()) {
            d->roomView->set_position_ms(ms);
        }
        if (is_pose3d_plugin()) {
            d->skeleton3dRoomView->set_position_ms(ms);
        }
        if (is_diarize_plugin()) {
            d->diarizeWaveform->set_playhead_ms(ms);
        }
    });

    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
}

// ── Session list ─────────────────────────────────────────────────────────

void AnalysisTabW::rebuild_session_list() {
    const QString selected = d->currentSessionPath;
    // Own directory first, then each admin-only extra directory (item 27)
    // — SessionInfo::list_all() itself stays a plain single-directory
    // scan, this just calls it once per directory and merges.
    d->sessions = SessionInfo::list_all(d->settings.record.directory);
    for (const QString& extraDir : d->extraDirectories) {
        d->sessions += SessionInfo::list_all(extraDir);
    }

    d->sessionList->blockSignals(true);
    d->sessionList->clear();
    int selectRow = -1;
    for (int i = 0; i < d->sessions.size(); ++i) {
        const auto& s = d->sessions[i];
        d->sessionList->addItem(
            QString("%1  (%2 cam, %3)").arg(s.name).arg(s.cameraCount).arg(s.format_duration()));
        if (s.path == selected) {
            selectRow = i;
        }
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
    const auto* info      = d->current_session();

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
    d->statusLbl->setStyleSheet("color:#6060a0; font-size:15px; font-weight:600;");

    select_camera(d->cameraCombo->currentIndex());
}

void AnalysisTabW::select_camera(int index) {
    Q_UNUSED(index);
    reload_current_camera_result();
}

void AnalysisTabW::select_plugin(int index) {
    if (index < 0) {
        return;
    }

    const bool isPose        = is_pose_plugin();
    const bool isDiarize     = is_diarize_plugin();
    const bool isExpression  = is_expression_plugin();
    const bool isFaceMask    = is_face_mask_plugin();
    const bool isGazeFusion  = is_gaze_fusion_plugin();
    const bool isPose3D      = is_pose3d_plugin();
    const bool isTriggerSync = is_trigger_sync_plugin();
    const bool isRppg        = is_rppg_plugin();
    const bool isGaze2d      = is_gaze2d_plugin();
    // A depth model selected within the Pose plugin produces a colorized
    // video, not keypoints — the keypoint/chart controls below need to stay
    // hidden for it, same as they are for every non-pose plugin.
    const bool isPoseKeypoints = isPose && !is_pose_depth_selected();

    // These are independent sibling widgets with overlapping visibility
    // rules, not QStackedWidget pages, so they aren't crossfaded as a group
    // (that would mean restructuring them into an actual stack). Hiding
    // stays instant; a widget that newly becomes visible gets a light fade-in
    // instead of popping — a widget already visible, or one being hidden,
    // is left alone.
    auto set_visible_animated = [](QWidget* w, bool visible) {
        if (visible == w->isVisible()) {
            return;
        }
        w->setVisible(visible);
        if (visible) {
            anim::fade_in_widget(w, 130);
        }
    };
    set_visible_animated(d->keypointFieldW, isPoseKeypoints);
    set_visible_animated(d->depthModeHintLbl, isPose && is_pose_depth_selected());
    set_visible_animated(d->blendshapeFieldW, isExpression);
    set_visible_animated(d->trackFieldW, isPose3D);
    // isPose3D included so the shared chart can show the Dyad Analysis
    // series — pose3d's own room-view/2D-overlay results view never needed
    // it before Dyad Analysis was added.
    set_visible_animated(d->chart,
                         isPoseKeypoints || isExpression || isRppg || isGaze2d || isPose3D);
    set_visible_animated(d->kinematicsRowW, isPoseKeypoints);
    // subjectPickerRowW's own further narrowing (hidden when the session has
    // <=1 detected subject) happens inside rebuild_subject_chips(), called
    // from reload_current_camera_result() right after this — this toggle
    // only handles "hide when leaving the Pose plugin entirely".
    if (!isPoseKeypoints) {
        set_visible_animated(d->subjectPickerRowW, false);
    }
    set_visible_animated(d->expressionRowW, isExpression);
    set_visible_animated(d->gazeFusionRowW, isGazeFusion);
    set_visible_animated(d->roomView, isGazeFusion);
    set_visible_animated(d->pose3dRowW, isPose3D);
    set_visible_animated(d->dyadRowW, isPose3D);
    set_visible_animated(d->skeleton3dRoomView, isPose3D);
    set_visible_animated(d->triggerSyncRowW, isTriggerSync);
    set_visible_animated(d->triggerSyncTable, isTriggerSync);
    set_visible_animated(d->rppgRowW, isRppg);
    set_visible_animated(d->gaze2dRowW, isGaze2d);
    set_visible_animated(d->openFolderBtn, isFaceMask || is_pose_depth_selected());
    set_visible_animated(d->sourceRowW, !isDiarize);
    set_visible_animated(d->micRowW, isDiarize);
    set_visible_animated(d->transcriptTable, isDiarize);
    set_visible_animated(d->diarizeWaveform, isDiarize);
    set_visible_animated(d->speakerLegendRowW, isDiarize);

    // Diarize's "video" is really a .wav — the video-surface area would
    // otherwise be a wasted black rectangle taking up roughly half the
    // results view. Collapse it to a slim playback strip (play/pause/
    // scrub/time only) and let transcriptTable's existing stretch factor
    // claim the freed width; every other plugin keeps today's unchanged
    // full-width behavior since both are restored here whenever isDiarize
    // is false.
    d->player->set_video_surface_visible(!isDiarize);
    d->player->setMaximumWidth(isDiarize ? 340 : QWIDGETSIZE_MAX);
    d->resultsSplitter->setStretchFactor(0, isDiarize ? 0 : 1);

    // controlsStack's own crossfade defers reload_current_camera_result()
    // until the new page is actually current (right as the fade-in phase
    // starts), matching LoginDialog::show_register_mode()'s existing
    // pattern of deferring a focus() call behind its own crossfade.
    anim::crossfade_stacked_widget(d->controlsStack, index, 130,
                                   [this] { reload_current_camera_result(); });
}

// Switching between a pose (keypoint) model and a depth model within the
// same Pose plugin page needs the same visibility reshaping select_plugin()
// already does for a *plugin* change. Reuses select_plugin() for that (it's
// idempotent — set_visible_animated() checks isVisible() first) rather than
// duplicating the toggle list here, but calls reload_current_camera_result()
// explicitly afterward: crossfade_stacked_widget() no-ops (and skips its
// onComplete callback) when the controlsStack page itself isn't changing —
// true here, since the plugin combo didn't move — so select_plugin() alone
// would leave the results view showing the previous model's stale output.
void AnalysisTabW::on_pose_model_changed() {
    if (!is_pose_plugin()) {
        return;
    }
    select_plugin(d->pluginCombo->currentIndex());
    reload_current_camera_result();
}

// ── Analysis lifecycle ───────────────────────────────────────────────────

QString AnalysisTabW::slug_for_model(const QString& modelId) const {
    // Strips just the trailing ".pt" (e.g. "yolov8n-pose.pt" -> "yolov8n-pose",
    // "yolo26n-depth.pt" -> "yolo26n-depth") — every current pose_model_options()/
    // pose_depth_model_options() entry is already filename-safe once that's
    // gone, so no further sanitization is needed.
    return QFileInfo(modelId).completeBaseName();
}

QString AnalysisTabW::pose_json_path_for(const QString& videoRelPath) const {
    // Own subfolder, not a sidecar beside the video (unlike transcript
    // below) — see analysis/run_pose.py::process_session()'s matching
    // pose_dir. Filename is namespaced by the currently-selected model
    // (see slug_for_model()) so running a second model against the same
    // session no longer silently overwrites the first — this also means
    // switching modelCombo doubles as "which saved result to view" for an
    // already-analyzed session, since on_pose_model_changed() already
    // triggers a reload on every combo change. Forward-only: sessions
    // analyzed before this change have their un-suffixed .pose.json under
    // video/ or pose/ instead and won't be found here until re-run,
    // matching this project's established no-migration convention for
    // directory/filename-layout changes (item 13).
    return "pose/" + QFileInfo(videoRelPath).completeBaseName() + "." +
           slug_for_model(d->modelCombo->currentData().toString()) + ".pose.json";
}

QString AnalysisTabW::anonymized_video_path_for(const QString& videoRelPath) const {
    return "anonymized/" + QFileInfo(videoRelPath).fileName();
}

QString AnalysisTabW::depth_video_path_for(const QString& videoRelPath) const {
    // Mirrors anonymized_video_path_for()'s own-subfolder convention — see
    // analysis/run_pose.py::process_session_depth()'s matching depth_dir.
    // Model-namespaced for the same reason/with the same forward-only
    // caveat as pose_json_path_for() above — the 3 depth variants (n/s/m)
    // shared the identical silent-overwrite gap.
    const QFileInfo info(videoRelPath);
    return "depth/" + info.completeBaseName() + "." +
           slug_for_model(d->modelCombo->currentData().toString()) + "." + info.suffix();
}

QString AnalysisTabW::transcript_json_path_for(const QString& audioRelPath) const {
    return sidecar_path_for(audioRelPath, ".transcript.json");
}

QString AnalysisTabW::expression_json_path_for(const QString& videoRelPath) const {
    // Own subfolder, not a sidecar beside the video — mirrors
    // pose_json_path_for()'s exact convention, see
    // analysis/run_expression.py::process_session()'s matching
    // expression_dir. Same forward-only caveat as pose_json_path_for().
    return "expression/" + QFileInfo(videoRelPath).completeBaseName() + ".expression.json";
}

QString AnalysisTabW::rppg_json_path_for(const QString& videoRelPath) const {
    // Own subfolder, backend-namespaced — mirrors pose_json_path_for()'s
    // exact convention (3 backends here instead of pose's model choice), see
    // analysis/run_rppg.py::_write_results(). Switching rppgBackendCombo
    // doubles as "which saved result to view" the same way switching
    // modelCombo does for Pose. Same forward-only caveat.
    return "rppg/" + QFileInfo(videoRelPath).completeBaseName() + "." +
           d->rppgBackendCombo->currentData().toString() + ".rppg.json";
}

QString AnalysisTabW::gaze2d_json_path_for(const QString& videoRelPath) const {
    // Own subfolder — mirrors pose_json_path_for()'s/expression_json_path_for()'s
    // exact convention, see analysis/run_gaze2d.py::process_session()'s
    // matching gaze2d_dir. No model/backend namespacing (unlike Pose's/
    // rPPG's) — there's only one algorithm. Same forward-only caveat as
    // every other subfolder-relocated plugin.
    return "gaze2d/" + QFileInfo(videoRelPath).completeBaseName() + ".gaze2d.json";
}

bool AnalysisTabW::is_pose_plugin() const {
    return d->pluginCombo->currentData().toString() == "pose";
}

bool AnalysisTabW::is_diarize_plugin() const {
    return d->pluginCombo->currentData().toString() == "diarize";
}

bool AnalysisTabW::is_expression_plugin() const {
    return d->pluginCombo->currentData().toString() == "expression";
}

bool AnalysisTabW::is_face_mask_plugin() const {
    return d->pluginCombo->currentData().toString() == "face_mask";
}

bool AnalysisTabW::is_gaze_fusion_plugin() const {
    return d->pluginCombo->currentData().toString() == "gaze_fusion";
}

bool AnalysisTabW::is_pose3d_plugin() const {
    return d->pluginCombo->currentData().toString() == "pose3d";
}

bool AnalysisTabW::is_trigger_sync_plugin() const {
    return d->pluginCombo->currentData().toString() == "trigger_sync";
}

bool AnalysisTabW::is_rppg_plugin() const {
    return d->pluginCombo->currentData().toString() == "rppg";
}

bool AnalysisTabW::is_gaze2d_plugin() const {
    return d->pluginCombo->currentData().toString() == "gaze2d";
}

bool AnalysisTabW::is_pose_depth_selected() const {
    return is_pose_plugin() && is_depth_model(d->modelCombo->currentData().toString());
}

void AnalysisTabW::reload_current_camera_result() {
    const auto* info = d->current_session();
    d->reset_all_results();

    if (is_gaze_fusion_plugin()) {
        // Unlike pose/expression/face_mask, this plugin's result is
        // session-level (fusion is inherently cross-camera), so it loads
        // regardless of whether a camera is selected — only the player's
        // per-camera 2D overlay needs cameraCombo's current selection.
        if (!info) {
            d->player->set_video(QString());
            d->player->set_pose_result(d->currentResult);
            d->roomView->set_result(d->currentGazeFusion);
            update_gaze_view();
            return;
        }

        const QString gazeAbs = info->path + "/gaze_fusion.json";
        d->currentGazeFusion =
            QFileInfo::exists(gazeAbs) ? GazeFusionResult::load(gazeAbs) : GazeFusionResult();
        d->roomView->set_result(d->currentGazeFusion);

        if (d->cameraCombo->currentIndex() >= 0) {
            const QString videoRel = d->cameraCombo->currentData().toString();
            d->player->set_video(info->path + "/" + videoRel);
            d->player->set_gaze_result(d->currentGazeFusion, d->cameraCombo->currentIndex());
        } else {
            d->player->set_video(QString());
            d->player->set_pose_result(d->currentResult);
        }

        update_gaze_view();

        if (!d->currentGazeFusion.is_valid()) {
            d->statusLbl->setText("No gaze fusion result yet for this session — click Run.");
            d->statusLbl->setStyleSheet("color:#6060a0; font-size:15px; font-weight:600;");
        }
        return;
    }

    if (is_pose3d_plugin()) {
        // Session-level result (reconstruction is inherently cross-camera),
        // so it loads regardless of whether a camera is selected — only the
        // player's per-camera reprojected overlay needs cameraCombo's
        // current selection. Mirrors the gaze_fusion branch above exactly.
        if (!info) {
            d->player->set_video(QString());
            d->player->set_pose_result(d->currentResult);
            d->skeleton3dRoomView->set_result(d->currentSkeleton3D);
            update_pose3d_view();
            return;
        }

        const QString skeletonAbs = info->path + "/skeleton3d.json";
        d->currentSkeleton3D = QFileInfo::exists(skeletonAbs) ? Skeleton3DResult::load(skeletonAbs)
                                                              : Skeleton3DResult();
        d->skeleton3dRoomView->set_result(d->currentSkeleton3D);

        if (d->cameraCombo->currentIndex() >= 0) {
            const QString videoRel = d->cameraCombo->currentData().toString();
            d->player->set_video(info->path + "/" + videoRel);
            d->player->set_skeleton3d_result(d->currentSkeleton3D, d->cameraCombo->currentIndex());
        } else {
            d->player->set_video(QString());
            d->player->set_pose_result(d->currentResult);
        }

        d->trackCombo->blockSignals(true);
        d->trackCombo->clear();
        d->trackCombo->addItem("All tracks", -1);
        QList<int> trackIds;
        for (const auto& frame : d->currentSkeleton3D.frames()) {
            for (const auto& person : frame.people) {
                if (!trackIds.contains(person.trackId)) {
                    trackIds << person.trackId;
                }
            }
        }
        std::sort(trackIds.begin(), trackIds.end());
        for (int tid : trackIds) {
            d->trackCombo->addItem(QString("Track %1").arg(tid), tid);
        }
        d->trackCombo->blockSignals(false);

        // Dyad Analysis track pickers — same trackIds list, real ids only
        // (no "All tracks" entry, a dyad needs exactly two specific people).
        // Default A/B to the first two distinct ids so the panel shows
        // something useful immediately rather than starting empty.
        d->dyadTrackACombo->blockSignals(true);
        d->dyadTrackBCombo->blockSignals(true);
        d->dyadTrackACombo->clear();
        d->dyadTrackBCombo->clear();
        for (int tid : trackIds) {
            d->dyadTrackACombo->addItem(QString("Track %1").arg(tid), tid);
            d->dyadTrackBCombo->addItem(QString("Track %1").arg(tid), tid);
        }
        const bool hasDyadPair = trackIds.size() >= 2;
        if (hasDyadPair) {
            d->dyadTrackACombo->setCurrentIndex(0);
            d->dyadTrackBCombo->setCurrentIndex(1);
        }
        d->dyadTrackACombo->setEnabled(hasDyadPair);
        d->dyadTrackBCombo->setEnabled(hasDyadPair);
        d->dyadMetricCombo->setEnabled(hasDyadPair);
        d->dyadWindowSpin->setEnabled(hasDyadPair);
        d->dyadTrackACombo->blockSignals(false);
        d->dyadTrackBCombo->blockSignals(false);

        update_pose3d_view();
        update_dyadic_view();

        if (!d->currentSkeleton3D.is_valid()) {
            d->statusLbl->setText("No 3D reconstruction yet for this session — click Run.");
            d->statusLbl->setStyleSheet("color:#6060a0; font-size:15px; font-weight:600;");
        }
        return;
    }

    if (is_trigger_sync_plugin()) {
        // Session-level result (trigger.csv/timestamps_camN.csv are read
        // together across the whole session), so it loads regardless of
        // whether a camera is selected — only the player's plain-video
        // playback (no overlay — nothing to draw) needs cameraCombo's
        // current selection, mirroring face_mask's plain-playback pattern.
        if (!info) {
            d->player->set_video(QString());
            d->player->set_pose_result(d->currentResult);
            update_trigger_sync_view();
            return;
        }

        d->currentTriggerFrameMap = TriggerFrameMap::load(info->path);

        if (d->cameraCombo->currentIndex() >= 0) {
            const QString videoRel = d->cameraCombo->currentData().toString();
            d->player->set_video(info->path + "/" + videoRel);
        } else {
            d->player->set_video(QString());
        }
        d->player->set_pose_result(d->currentResult); // no overlay for this plugin

        update_trigger_sync_view();

        if (!d->currentTriggerFrameMap.is_valid()) {
            d->statusLbl->setText("No trigger/frame sync yet for this session — click Run.");
            d->statusLbl->setStyleSheet("color:#6060a0; font-size:15px; font-weight:600;");
        }
        return;
    }

    if (is_diarize_plugin()) {
        if (!info || d->micCombo->currentIndex() < 0) {
            d->player->set_video(QString()); // stop/clear any previously-loaded audio
            d->player->set_pose_result(d->currentResult);
            d->diarizeWaveform->clear_static_envelope();
            d->diarizeWaveform->set_speaker_bands({});
            rebuild_speaker_legend();
            update_transcript_table();
            return;
        }

        const QString audioRel = d->micCombo->currentData().toString();
        const QString audioAbs = info->path + "/" + audioRel;
        d->player->set_video(audioAbs);
        d->player->set_pose_result(d->currentResult);

        // Whole-clip envelope for the waveform strip, spanning the full
        // duration — a nicer, more informative view alongside the transcript
        // than the bare player controls alone. 600 columns is plenty of
        // horizontal resolution for this widget's typical width and cheap to
        // compute even for a long recording (streamed one column at a time,
        // never the whole file in memory at once).
        const auto wav = load_wav_envelope(audioAbs, 600);
        d->diarizeWaveform->set_static_envelope(wav.columns, wav.durationMs);

        const QString transcriptAbs = info->path + "/" + transcript_json_path_for(audioRel);
        d->currentTranscript        = QFileInfo::exists(transcriptAbs)
                                          ? TranscriptResult::load(transcriptAbs)
                                          : TranscriptResult();

        QVector<AudioWaveformW::SpeakerBand> bands;
        bands.reserve(d->currentTranscript.segments().size());
        for (const auto& seg : d->currentTranscript.segments()) {
            bands.push_back({seg.startMs, seg.endMs, seg.speaker});
        }
        d->diarizeWaveform->set_speaker_bands(bands);
        rebuild_speaker_legend();
        update_transcript_table();

        if (!d->currentTranscript.is_valid()) {
            d->statusLbl->setText("No transcript yet for this mic — click Run.");
            d->statusLbl->setStyleSheet("color:#6060a0; font-size:15px; font-weight:600;");
        } else if (!d->currentTranscript.has_diarization()) {
            d->statusLbl->setText(
                "Transcript loaded (no speaker labels — diarization was skipped or unavailable).");
            d->statusLbl->setStyleSheet("color:#6060a0; font-size:15px; font-weight:600;");
        }
        return;
    }

    if (!info || d->cameraCombo->currentIndex() < 0) {
        d->player->set_video(QString()); // stop/clear any previously-loaded video
        d->player->set_pose_result(d->currentResult);
        rebuild_subject_chips(0); // no session selected — no chips to show
        d->keypointCombo->clear();
        d->blendshapeCombo->clear();
        // Explicit calls rather than relying on QComboBox::clear() above
        // reentrantly firing currentIndexChanged(-1) into
        // update_kinematics_chart()/update_expression_view() — that happens
        // to reset the chart/stats today, but only as an accidental side
        // effect of not being wrapped in blockSignals() the way the
        // populated-combo case below is.
        update_kinematics_chart();
        update_expression_view();
        update_rppg_view();
        update_gaze2d_view();
        return;
    }

    const QString videoRel = d->cameraCombo->currentData().toString();
    const QString videoAbs = info->path + "/" + videoRel;

    if (is_pose_plugin()) {
        if (is_pose_depth_selected()) {
            // Depth mode: plain video playback of the colorized depth-map
            // output, no keypoint overlay/chart — mirrors the Face Masking
            // plugin's anonymized-video branch below exactly, just against
            // a sibling depth/ folder instead of anonymized/.
            const QString depthAbs = info->path + "/" + depth_video_path_for(videoRel);
            const bool hasOutput   = QFileInfo::exists(depthAbs);
            d->player->set_video(hasOutput ? depthAbs : videoAbs);
            d->player->set_pose_result(PoseAnalysisResult()); // no overlay in depth mode
            rebuild_subject_chips(0);                         // depth mode has no per-subject chart
            d->openFolderBtn->setEnabled(hasOutput);

            if (hasOutput) {
                d->statusLbl->setText("Showing depth-map output.");
                d->statusLbl->setStyleSheet("color:#44cc66; font-size:15px; font-weight:600;");
            } else {
                d->statusLbl->setText("Not yet computed (showing original) — click Run.");
                d->statusLbl->setStyleSheet("color:#6060a0; font-size:15px; font-weight:600;");
            }
            return;
        }

        d->player->set_video(videoAbs);

        const QString poseAbs = info->path + "/" + pose_json_path_for(videoRel);
        d->currentResult =
            QFileInfo::exists(poseAbs) ? PoseAnalysisResult::load(poseAbs) : PoseAnalysisResult();
        d->player->set_pose_result(d->currentResult);

        d->keypointCombo->blockSignals(true);
        d->keypointCombo->clear();
        for (const auto& name : d->currentResult.keypoint_names()) {
            d->keypointCombo->addItem(name);
        }
        d->keypointCombo->blockSignals(false);

        rebuild_subject_chips(max_subjects_in(d->currentResult));
        update_kinematics_chart();

        // Only overwrite statusLbl for the "nothing to show yet" cases — a
        // valid result WITH detections leaves whatever's already there
        // alone, so a "Done." set by the analysis_finished handler right
        // before this runs (via rebuild_session_list()) isn't immediately
        // clobbered back to a generic "Ready." in the same call stack. A
        // valid result with zero detections is different: that's more
        // useful information than "Done." on its own, so it does overwrite.
        if (!d->currentResult.is_valid()) {
            // pose_json_path_for() is model-namespaced, so "invalid" here
            // means either "never analyzed at all" or "analyzed with a
            // different model" — check the pose/ folder for any OTHER
            // model's result for this same video to tell those apart, since
            // the second case has a clearer fix (switch models above)
            // rather than implying nothing has been done for this camera.
            const QDir poseDir(info->path + "/pose");
            const QString stem = QFileInfo(videoRel).completeBaseName();
            const bool otherModelExists =
                !poseDir.entryList({stem + ".*.pose.json"}, QDir::Files).isEmpty();
            if (otherModelExists) {
                d->statusLbl->setText(
                    "This camera hasn't been analyzed with the selected "
                    "model yet — click Run, or pick a model above that's "
                    "already been run for this session.");
            } else {
                d->statusLbl->setText("No analysis yet for this camera — click Run.");
            }
            d->statusLbl->setStyleSheet("color:#6060a0; font-size:15px; font-weight:600;");
        } else if (!d->currentResult.has_any_detections()) {
            d->statusLbl->setText(
                "Pose ran, but no person was detected in this camera's "
                "footage — try a different camera, or check its framing/lighting.");
            d->statusLbl->setStyleSheet("color:#ddaa33; font-size:15px; font-weight:600;");
        }
        return;
    }

    if (is_expression_plugin()) {
        d->player->set_video(videoAbs);

        const QString exprAbs = info->path + "/" + expression_json_path_for(videoRel);
        d->currentExpressionResult =
            QFileInfo::exists(exprAbs) ? ExpressionResult::load(exprAbs) : ExpressionResult();
        d->player->set_expression_result(d->currentExpressionResult);

        d->blendshapeCombo->blockSignals(true);
        d->blendshapeCombo->clear();
        for (const auto& name : d->currentExpressionResult.blendshape_names()) {
            d->blendshapeCombo->addItem(name);
        }
        d->blendshapeCombo->blockSignals(false);

        update_expression_view();

        // frames().isEmpty() alongside is_valid() matches the pose branch's
        // guard above — a malformed/partial result file could parse with
        // is_valid()==true but zero frames (see ExpressionResult::load()),
        // and this message should fire for that case too, not just a
        // missing file.
        const auto& exprResult = d->currentExpressionResult;
        if (!exprResult.is_valid() || exprResult.frames().isEmpty()) {
            d->statusLbl->setText("No analysis yet for this camera — click Run.");
            d->statusLbl->setStyleSheet("color:#6060a0; font-size:15px; font-weight:600;");
        } else if (!exprResult.has_any_detections()) {
            d->statusLbl->setText(
                "Expression ran, but no face was detected in this camera's "
                "footage — try a different camera, or check its framing/lighting.");
            d->statusLbl->setStyleSheet("color:#ddaa33; font-size:15px; font-weight:600;");
        }
        return;
    }

    if (is_rppg_plugin()) {
        d->player->set_video(videoAbs);

        const QString rppgAbs = info->path + "/" + rppg_json_path_for(videoRel);
        d->currentRppgResult =
            QFileInfo::exists(rppgAbs) ? RppgResult::load(rppgAbs) : RppgResult();
        d->player->set_rppg_result(d->currentRppgResult, d->cameraCombo->currentIndex());

        update_rppg_view();

        if (!d->currentRppgResult.is_valid() || d->currentRppgResult.windows().isEmpty()) {
            d->statusLbl->setText("No analysis yet for this camera — click Run.");
            d->statusLbl->setStyleSheet("color:#6060a0; font-size:15px; font-weight:600;");
        } else if (d->currentRppgResult.pct_windows_good() <= 0.0) {
            d->statusLbl->setText(
                "rPPG ran, but no window had a reliable estimate in this "
                "camera's footage — try a different camera, better lighting, "
                "or check the subject held still and faced the camera.");
            d->statusLbl->setStyleSheet("color:#ddaa33; font-size:15px; font-weight:600;");
        }
        return;
    }

    if (is_gaze2d_plugin()) {
        d->player->set_video(videoAbs);

        const QString gaze2dAbs = info->path + "/" + gaze2d_json_path_for(videoRel);
        d->currentGaze2dResult =
            QFileInfo::exists(gaze2dAbs) ? Gaze2dResult::load(gaze2dAbs) : Gaze2dResult();
        d->player->set_gaze2d_result(d->currentGaze2dResult, d->cameraCombo->currentIndex());

        update_gaze2d_view();

        if (!d->currentGaze2dResult.is_valid() || d->currentGaze2dResult.frames().isEmpty()) {
            d->statusLbl->setText("No analysis yet for this camera — click Run.");
            d->statusLbl->setStyleSheet("color:#6060a0; font-size:15px; font-weight:600;");
        } else if (d->currentGaze2dResult.pct_frames_with_face() <= 0.0) {
            d->statusLbl->setText(
                "2D Gaze ran, but no face was detected in this camera's "
                "footage — try a different camera, or check its framing/lighting.");
            d->statusLbl->setStyleSheet("color:#ddaa33; font-size:15px; font-weight:600;");
        }
        return;
    }

    if (is_face_mask_plugin()) {
        // No keypoint/chart data, just plain video playback — the
        // anonymized output if it exists yet, otherwise the original as a
        // preview so the user can confirm they picked the right
        // camera/session.
        const QString anonAbs = info->path + "/" + anonymized_video_path_for(videoRel);
        const bool hasOutput  = QFileInfo::exists(anonAbs);
        d->player->set_video(hasOutput ? anonAbs : videoAbs);
        d->player->set_pose_result(d->currentResult);
        d->openFolderBtn->setEnabled(hasOutput);

        if (hasOutput) {
            d->statusLbl->setText("Showing anonymized output.");
            d->statusLbl->setStyleSheet("color:#44cc66; font-size:15px; font-weight:600;");
        } else {
            d->statusLbl->setText("Not yet anonymized (showing original) — click Run.");
            d->statusLbl->setStyleSheet("color:#6060a0; font-size:15px; font-weight:600;");
        }
        return;
    }

    // Defensive: pluginCombo only ever offers the ids handled above, but an
    // unconditional fallthrough here would otherwise silently show
    // face-mask's view for any future unmatched plugin id — the same
    // "leftover = face_mask" gap run_analysis() was fixed to avoid.
    d->statusLbl->setText("Error: unknown plugin selected.");
    d->statusLbl->setStyleSheet("color:#cc4444; font-size:15px; font-weight:600;");
}

// ── Kinematics view ──────────────────────────────────────────────────────

// Rebuilds the subject-chip row for a newly-loaded Pose result. subjectCount
// <= 1 leaves the row empty and hidden — nothing to pick between, matching
// this file's established "don't show a control with nothing to control"
// convention (e.g. trackFieldW for a single-track pose3d session). Chip 0
// defaults checked, matching the single-subject view every session showed
// before this feature existed.
void AnalysisTabW::rebuild_subject_chips(int subjectCount) {
    auto* layout      = d->subjectPickerRowW->layout();
    QLayoutItem* item = nullptr;
    while ((item = layout->takeAt(0)) != nullptr) {
        if (auto* w = item->widget()) {
            w->deleteLater();
        }
        delete item;
    }
    d->subjectChips.clear();

    if (subjectCount <= 1) {
        d->subjectPickerRowW->setVisible(false);
        return;
    }

    layout->addWidget(new QLabel("Subjects:"));
    for (int i = 0; i < subjectCount; ++i) {
        auto* chip = new QToolButton;
        chip->setText(QString("Subject %1").arg(i + 1));
        chip->setCheckable(true);
        chip->setChecked(i == 0);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolTip(
            "Subject identity is not tracked across frames — this is per-frame "
            "detection order, not a tracked individual.");
        const QString hex = subject_color(i).name();
        chip->setStyleSheet(
            QString("QToolButton { border: 2px solid %1; border-radius: 4px; padding: 3px 8px;"
                    "  color: %1; background: transparent; }"
                    "QToolButton:checked { background: %1; color: #0a0a1a; }")
                .arg(hex));
        connect(chip, &QToolButton::toggled, this, &AnalysisTabW::update_kinematics_chart);
        layout->addWidget(chip);
        d->subjectChips.push_back(chip);
    }
    static_cast<QHBoxLayout*>(layout)->addStretch();
    d->subjectPickerRowW->setVisible(is_pose_plugin());
}

QVector<int> AnalysisTabW::checked_subject_indices() const {
    QVector<int> out;
    for (int i = 0; i < d->subjectChips.size(); ++i) {
        if (d->subjectChips[i]->isChecked()) {
            out.push_back(i);
        }
    }
    if (out.isEmpty()) {
        out.push_back(0);
    } // defensive fallback — chart is never silently empty
    return out;
}

void AnalysisTabW::update_kinematics_chart() {
    const int keypointIndex     = d->keypointCombo->currentIndex();
    const QString metric        = d->metricCombo->currentData().toString();
    const QString keypointName  = keypointIndex >= 0 ? d->keypointCombo->currentText() : QString();
    const QVector<int> subjects = checked_subject_indices();

    if (metric == "position" || keypointIndex < 0) {
        d->chart->set_multi_subject_position(d->currentResult, keypointIndex, subjects);
        d->chart->set_title(keypointName.isEmpty() ? "No keypoint selected"
                                                   : keypointName + " — Position");
        d->kinematicsStatsLbl->clear();
        d->chart->set_playhead_ms(d->player->position_ms());
        return;
    }

    const bool isSpeed       = metric == "speed";
    const double scale       = d->scaleSpin->value(); // mm/px, 1.0 = raw pixels
    const bool isMm          = scale != 1.0;
    const QString metricName = isSpeed ? "Speed" : "Acceleration";
    const QString unit       = isSpeed ? (isMm ? "mm/s" : "px/s") : (isMm ? "mm/s²" : "px/s²");
    const int64_t t0         = d->first_timestamp_ns();

    QVector<QPair<int, QVector<QPointF>>> perSubject;
    QStringList statLines;
    for (int subjectIndex : subjects) {
        const auto series = compute_kinematics(d->currentResult, keypointIndex, subjectIndex,
                                               d->smoothingSpin->value());
        QVector<QPointF> points;
        points.reserve(series.samples.size());
        for (const auto& sample : series.samples) {
            const double value = isSpeed ? sample.speedPxPerS : sample.accelPxPerS2;
            if (std::isnan(value)) {
                continue;
            }
            const double tMs = (sample.timestampNs - t0) / 1e6;
            points.append(QPointF(tMs, value * scale));
        }
        perSubject.push_back({subjectIndex, points});

        if (std::isnan(series.stats.avgSpeedPxPerS)) {
            statLines << QString("Subject %1: not enough data").arg(subjectIndex + 1);
        } else {
            statLines << QString("Subject %1 — dist %2  avg %3 %4  max %5 %4")
                             .arg(subjectIndex + 1)
                             .arg(series.stats.totalDistancePx * scale, 0, 'f', 1)
                             .arg(series.stats.avgSpeedPxPerS * scale, 0, 'f', 1)
                             .arg(isMm ? "mm/s" : "px/s")
                             .arg(series.stats.maxSpeedPxPerS * scale, 0, 'f', 1);
        }
    }

    d->chart->set_multi_subject_series(perSubject, QString("%1 (%2)").arg(metricName, unit));
    d->chart->set_title(keypointName + " — " + metricName);
    d->chart->set_playhead_ms(d->player->position_ms());
    d->kinematicsStatsLbl->setText(statLines.join("   |   "));
}

void AnalysisTabW::export_kinematics_csv() {
    const auto* info = d->current_session();
    if (!info) {
        return;
    }

    const QString modelId   = d->modelCombo->currentData().toString();
    const QString modelSlug = slug_for_model(modelId);
    // Own subfolder (mirrors pose/, depth/, expression/, anonymized/) — one
    // file per model, covering every keypoint and every camera, rather
    // than requiring one export click per keypoint currently selected in
    // the chart. QFile::open() doesn't create missing parent directories,
    // so this folder is created up front, same as every Python-side output
    // folder already does via mkdir(parents=True).
    QDir(info->path).mkpath("kinematics");
    const QString suggested = info->path + "/kinematics/" + modelSlug + ".csv";

    // Always raw pixel units, regardless of the display-layer mm/px scale —
    // unambiguous for anyone reading the file without knowing what scale
    // was selected in the UI at export time.
    const int smoothingWindow   = d->smoothingSpin->value();
    const QVector<int> subjects = checked_subject_indices();

    // Load every camera's own result for the currently-selected model
    // (pose_json_path_for() is model-aware) up front — both to skip
    // cameras with nothing to export, and to compute one SHARED t0 (the
    // earliest first-frame timestamp across all of them) rather than each
    // camera resetting to its own zero. All cameras in a session already
    // share one process-wide clock origin (elapsed_ns()), so a shared t0
    // keeps the exported timestamp_ms values directly comparable across
    // cameras instead of each column restarting at its own camera's start.
    struct CameraResult {
        QString label;
        PoseAnalysisResult result;
    };
    QVector<CameraResult> perCamera;
    int64_t t0 = std::numeric_limits<int64_t>::max();
    for (int camIdx = 0; camIdx < info->videoFiles.size(); ++camIdx) {
        const QString poseAbs = info->path + "/" + pose_json_path_for(info->videoFiles[camIdx]);
        auto result           = PoseAnalysisResult::load(poseAbs);
        if (!result.is_valid() || result.frames().isEmpty()) {
            continue;
        }
        t0 = std::min(t0, result.frames().first().timestampNs);
        perCamera.push_back({QString("Camera %1").arg(camIdx), std::move(result)});
    }
    if (perCamera.isEmpty()) {
        return;
    }

    export_csv(this, "Export Kinematics", suggested, [&](QTextStream& ts) {
        // Caveats below matter to anyone reading this file without having
        // seen the app: subject numbers are per-frame detection order, not
        // identity tracked frame-to-frame OR across cameras (a stat like
        // max speed can be corrupted by an identity swap in a multi-subject
        // session, and "subject 0" in one camera is not necessarily the
        // same physical person as "subject 0" in another), and x_px/y_px
        // are post-smoothing, not the raw detector output.
        ts << "# subject numbers are per-frame detection order, not tracked identity "
              "across frames or cameras — see the in-app tooltip on the Subject chips\n";
        ts << "# model: " << modelId << "\n";
        ts << "# smoothing_window=" << smoothingWindow
           << " (x_px/y_px below are post-smoothing positions)\n";
        ts << "camera,keypoint,subject,timestamp_ms,x_px,y_px,speed_px_s,accel_px_s2\n";
        for (const auto& cam : perCamera) {
            // Every keypoint the model reports (e.g. all 17 COCO keypoints),
            // not just whichever one happens to be selected in the chart —
            // each camera's own result is the source of truth for its
            // keypoint list/order, not the (single, currently-displayed
            // camera's) keypointCombo.
            const QStringList& keypointNames = cam.result.keypoint_names();
            for (int keypointIndex = 0; keypointIndex < keypointNames.size(); ++keypointIndex) {
                for (int subjectIndex : subjects) {
                    const auto series = compute_kinematics(cam.result, keypointIndex, subjectIndex,
                                                           smoothingWindow);
                    for (const auto& sample : series.samples) {
                        ts << cam.label << "," << keypointNames[keypointIndex] << ","
                           << (subjectIndex + 1) << "," << (sample.timestampNs - t0) / 1e6 << ","
                           << sample.position.x() << "," << sample.position.y() << ","
                           << (std::isnan(sample.speedPxPerS) ? QString()
                                                              : QString::number(sample.speedPxPerS))
                           << ","
                           << (std::isnan(sample.accelPxPerS2)
                                   ? QString()
                                   : QString::number(sample.accelPxPerS2))
                           << "\n";
                    }
                }
            }
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

        auto* speakerItem =
            new QTableWidgetItem(seg.speaker.isEmpty() ? QString("—") : seg.speaker);
        speakerItem->setFlags(speakerItem->flags() & ~Qt::ItemIsEditable);
        // Colors the text to match the waveform's speaker bands and the
        // legend row, and bolds it so speaker identity pops at a glance.
        const QColor speakerColor = d->diarizeWaveform->speaker_color(seg.speaker);
        speakerItem->setForeground(QBrush(speakerColor));
        QFont speakerFont = speakerItem->font();
        speakerFont.setBold(true);
        speakerItem->setFont(speakerFont);
        d->transcriptTable->setItem(i, 2, speakerItem);

        auto* textItem = new QTableWidgetItem(seg.text);
        textItem->setFlags(textItem->flags() & ~Qt::ItemIsEditable);
        d->transcriptTable->setItem(i, 3, textItem);

        // A low-alpha per-row tint (across all 4 cells) matching the
        // speaker's own color — subtle enough to not fight QTableWidget's
        // selection highlight, unlike a full-strength background would.
        // Rows with no attributed speaker keep the default background (no
        // fabricated color), matching the waveform's own "gaps draw
        // neither wash nor strip" discipline.
        if (!seg.speaker.isEmpty()) {
            QColor tint = speakerColor;
            tint.setAlpha(22);
            const QBrush tintBrush(tint);
            startItem->setBackground(tintBrush);
            endItem->setBackground(tintBrush);
            speakerItem->setBackground(tintBrush);
            textItem->setBackground(tintBrush);
        }
    }
    d->transcriptTable->resizeRowsToContents();

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
    d->transcriptStatsLbl->setText(
        QString("%1 segment(s)  ·  %2").arg(segments.size()).arg(suffix));
}

// Rebuilds the small color-swatch legend row from d->diarizeWaveform's own
// speaker->color assignment (set by the most recent set_speaker_bands()
// call) so the legend, waveform bands, and transcript table's Speaker
// column text all agree on the same mapping. Stays empty (no chips) for a
// transcript-only session with no diarization.
void AnalysisTabW::rebuild_speaker_legend() {
    auto* layout      = d->speakerLegendRowW->layout();
    QLayoutItem* item = nullptr;
    while ((item = layout->takeAt(0)) != nullptr) {
        if (auto* w = item->widget()) {
            w->deleteLater();
        }
        delete item;
    }

    for (const auto& [speaker, color] : d->diarizeWaveform->speaker_legend()) {
        auto* chip = new QWidget;
        chip->setStyleSheet("background: rgba(255,255,255,12); border-radius: 4px;");
        auto* chipLay = new QHBoxLayout(chip);
        chipLay->setContentsMargins(6, 2, 6, 2);
        chipLay->setSpacing(6);

        auto* swatch = new QLabel;
        swatch->setFixedSize(14, 14);
        swatch->setStyleSheet(QString("background: %1; border-radius: 4px;").arg(color.name()));
        chipLay->addWidget(swatch);

        auto* label = new QLabel(speaker);
        label->setStyleSheet("color:#a0a0c8; font-size:12px;");
        chipLay->addWidget(label);

        layout->addWidget(chip);
    }
    static_cast<QHBoxLayout*>(layout)->addStretch();
}

void AnalysisTabW::highlight_active_transcript_row(int64_t ms) {
    if (!is_diarize_plugin()) {
        return;
    }

    const auto& segments = d->currentTranscript.segments();
    const auto* seg      = d->currentTranscript.segment_at(ms);
    if (!seg) {
        return;
    }

    // segment_at() returns a pointer into this exact vector, and
    // update_transcript_table() populates rows 1:1 in the same order, so the
    // row index is just the pointer's offset — no need to scan the table.
    const int row = static_cast<int>(seg - segments.constData());
    if (row < 0 || row >= d->transcriptTable->rowCount()) {
        return;
    }

    if (d->transcriptTable->currentRow() != row) {
        d->transcriptTable->selectRow(row);
        if (auto* item = d->transcriptTable->item(row, 0)) {
            d->transcriptTable->scrollToItem(item, QAbstractItemView::PositionAtCenter);
        }
    }
}

void AnalysisTabW::export_transcript_csv() {
    const auto* info = d->current_session();
    if (!info || !d->currentTranscript.is_valid()) {
        return;
    }

    const QString micLabel  = d->micCombo->currentText();
    const QString suggested = info->path + "/" + micLabel + "_transcript.csv";

    export_csv(this, "Export Transcript", suggested, [&](QTextStream& ts) {
        if (!d->currentTranscript.has_diarization()) {
            ts << "# diarization=skipped - the speaker column is blank for every row\n";
        }
        ts << "start_s,end_s,speaker,text\n";
        for (const auto& seg : d->currentTranscript.segments()) {
            QString text = seg.text;
            text.replace('"', "\"\""); // minimal CSV escaping — text may contain commas
            ts << (seg.startMs / 1000.0) << "," << (seg.endMs / 1000.0) << "," << seg.speaker
               << ",\"" << text << "\"\n";
        }
    });
}

// ── Expression view ──────────────────────────────────────────────────────

void AnalysisTabW::update_expression_view() {
    if (!d->currentExpressionResult.is_valid()) {
        d->chart->set_single_series({}, "Score (0–1)", "Blendshape score");
        d->chart->set_title("No analysis yet");
        d->expressionStatsLbl->clear();
        d->chart->set_playhead_ms(d->player->position_ms());
        return;
    }

    // The %-per-category stats below don't depend on which blendshape is
    // selected — only the chart series does — so an empty/unselected
    // blendshapeCombo (e.g. a result with no blendshape_names) still gets a
    // populated stats label, just an empty chart.
    const int blendshapeIndex = d->blendshapeCombo->currentIndex();
    const QString blendshapeName =
        blendshapeIndex >= 0 ? d->blendshapeCombo->currentText() : QString();

    QVector<QPointF> points;
    const auto& frames = d->currentExpressionResult.frames();
    const int64_t t0   = frames.isEmpty() ? 0 : frames.first().timestampNs;

    // %-of-analyzed-frames per dominant_expression, using subject 0 only —
    // same "no cross-frame identity tracking" caveat as PoseSubject, see
    // expressionStatsLbl's tooltip.
    QMap<QString, int> categoryCounts;
    int totalWithSubject = 0;

    for (const auto& frame : frames) {
        if (frame.subjects.isEmpty()) {
            continue;
        }
        const auto& subject = frame.subjects.first();
        if (blendshapeIndex >= 0 && blendshapeIndex < subject.blendshapeScores.size()) {
            const double tMs = (frame.timestampNs - t0) / 1e6;
            points.append(QPointF(tMs, subject.blendshapeScores[blendshapeIndex]));
        }
        categoryCounts[subject.dominantExpression]++;
        ++totalWithSubject;
    }

    // Floors the axis at the last ANALYZED frame's time (regardless of
    // whether a face was detected there), not just the last PLOTTED point —
    // see set_single_series()'s minDurationMs doc comment for why this
    // matters whenever detection drops out before the video ends.
    const double lastAnalyzedMs =
        frames.isEmpty() ? 0.0 : static_cast<double>(frames.last().timestampNs - t0) / 1e6;
    d->chart->set_single_series(points, "Score (0–1)",
                                blendshapeName.isEmpty() ? "Blendshape score" : blendshapeName,
                                lastAnalyzedMs);
    d->chart->set_title(blendshapeName.isEmpty() ? "Facial expression" : blendshapeName);
    d->chart->set_playhead_ms(d->player->position_ms());

    if (totalWithSubject == 0) {
        d->expressionStatsLbl->setText("No detected faces in this result.");
        return;
    }
    QStringList parts;
    for (auto it = categoryCounts.constBegin(); it != categoryCounts.constEnd(); ++it) {
        parts << QString("%1 %2%").arg(it.key()).arg(100.0 * it.value() / totalWithSubject, 0, 'f',
                                                     0);
    }
    d->expressionStatsLbl->setText(parts.join("  ·  "));
}

void AnalysisTabW::export_expression_csv() {
    const auto* info = d->current_session();
    if (!info || !d->currentExpressionResult.is_valid()) {
        return;
    }

    const QString cameraLabel = d->cameraCombo->currentText();
    const QString suggested   = info->path + "/" + cameraLabel + "_expression.csv";
    const auto& names         = d->currentExpressionResult.blendshape_names();
    const auto& frames        = d->currentExpressionResult.frames();
    const int64_t t0          = frames.isEmpty() ? 0 : frames.first().timestampNs;

    export_csv(this, "Export Expression", suggested, [&](QTextStream& ts) {
        ts << "# subject_id is not tracked across frames — treat as one "
              "continuous face only for single-face sessions\n";
        ts << "timestamp_ms,subject_id,confidence,bbox_x1,bbox_y1,bbox_x2,bbox_y2,"
              "dominant_expression,dominant_score";
        for (const auto& n : names) {
            ts << "," << n;
        }
        // AU columns only exist for the py-feat backend — au_names() is
        // empty for heuristic/FER+ results, so this loop (and the matching
        // per-row loop below) is a pure no-op for those, keeping their
        // exported CSV byte-identical to before this field existed.
        for (const auto& n : d->currentExpressionResult.au_names()) {
            ts << "," << n;
        }
        ts << "\n";
        for (const auto& frame : frames) {
            const int64_t tMs = (frame.timestampNs - t0) / 1000000;
            for (const auto& s : frame.subjects) {
                ts << tMs << "," << s.subjectId << "," << s.confidence << "," << s.bbox.left()
                   << "," << s.bbox.top() << "," << s.bbox.right() << "," << s.bbox.bottom() << ","
                   << s.dominantExpression << "," << s.dominantScore;
                for (double v : s.blendshapeScores) {
                    ts << "," << v;
                }
                for (double v : s.actionUnits) {
                    ts << "," << v;
                }
                ts << "\n";
            }
        }
    });
}

// ── Gaze fusion view ─────────────────────────────────────────────────────

void AnalysisTabW::update_gaze_view() {
    if (!d->currentGazeFusion.is_valid() || d->currentGazeFusion.frames().isEmpty()) {
        d->gazeStatsLbl->clear();
        return;
    }

    const auto& frames = d->currentGazeFusion.frames();
    int nTriangulated = 0, nWithTarget = 0;
    double residualSum = 0.0;
    int residualCount  = 0;
    for (const auto& f : frames) {
        if (f.isTriangulated) {
            ++nTriangulated;
            if (f.residualRmsMm >= 0.0) {
                residualSum += f.residualRmsMm;
                ++residualCount;
            }
        }
        if (f.hasTarget) {
            ++nWithTarget;
        }
    }

    // "Triangulated" always means >=2 contributing cameras (the mathematical
    // minimum closest_point_of_rays() needs) — a fixed threshold in
    // run_gaze_fusion.py, independent of "min cams". The "min cams" spinbox
    // only gates the separately-reported target-point ("% with a valid
    // target" below); don't conflate the two in this label.
    const double avgResidual = residualCount > 0 ? residualSum / residualCount : 0.0;
    d->gazeStatsLbl->setText(
        QString("%1 frame(s)  ·  %2% triangulated (≥2 cams)  ·  avg residual %3 mm  ·  "
                "%4% with a valid target")
            .arg(frames.size())
            .arg(100.0 * nTriangulated / frames.size(), 0, 'f', 0)
            .arg(avgResidual, 0, 'f', 1)
            .arg(100.0 * nWithTarget / frames.size(), 0, 'f', 0));
}

void AnalysisTabW::export_gaze_csv() {
    const auto* info = d->current_session();
    if (!info || !d->currentGazeFusion.is_valid()) {
        return;
    }

    const QString suggested = info->path + "/gaze_fusion.csv";

    export_csv(this, "Export Gaze Fusion", suggested, [&](QTextStream& ts) {
        ts << "# subject_id is not tracked across frames — treat subject 0 as one "
              "continuous face only for single-face sessions\n";
        ts << "tick,timestamp_ns,num_cameras,is_triangulated,"
              "fused_origin_x,fused_origin_y,fused_origin_z,"
              "fused_direction_x,fused_direction_y,fused_direction_z,"
              "residual_rms_mm,target_x,target_y,target_z\n";
        for (const auto& f : d->currentGazeFusion.frames()) {
            ts << f.tick << "," << f.timestampNs << "," << f.numCameras << ","
               << (f.isTriangulated ? "1" : "0") << "," << f.fusedOriginRoom[0] << ","
               << f.fusedOriginRoom[1] << "," << f.fusedOriginRoom[2] << ","
               << f.fusedDirectionRoom[0] << "," << f.fusedDirectionRoom[1] << ","
               << f.fusedDirectionRoom[2] << ","
               << (f.residualRmsMm >= 0.0 ? QString::number(f.residualRmsMm) : QString()) << ","
               << (f.hasTarget ? QString::number(f.targetPointRoom[0]) : QString()) << ","
               << (f.hasTarget ? QString::number(f.targetPointRoom[1]) : QString()) << ","
               << (f.hasTarget ? QString::number(f.targetPointRoom[2]) : QString()) << "\n";
        }
    });
}

// ── 3D Pose Reconstruction view ──────────────────────────────────────────

void AnalysisTabW::update_pose3d_view() {
    if (!d->currentSkeleton3D.is_valid() || d->currentSkeleton3D.frames().isEmpty()) {
        d->pose3dStatsLbl->clear();
        return;
    }

    const int trackFilter =
        d->trackCombo->currentIndex() >= 0 ? d->trackCombo->currentData().toInt() : -1;

    int nFramesWithPeople = 0;
    int nPeopleTotal      = 0;
    double errorSum       = 0.0;
    int errorCount        = 0;
    int validKpSum        = 0;
    int totalKpSum        = 0;

    for (const auto& frame : d->currentSkeleton3D.frames()) {
        bool anyThisFrame = false;
        for (const auto& person : frame.people) {
            if (trackFilter >= 0 && person.trackId != trackFilter) {
                continue;
            }
            anyThisFrame = true;
            ++nPeopleTotal;
            for (const auto& kp : person.keypoints) {
                ++totalKpSum;
                if (kp.valid) {
                    ++validKpSum;
                    errorSum += kp.reprojectionErrorPx;
                    ++errorCount;
                }
            }
        }
        if (anyThisFrame) {
            ++nFramesWithPeople;
        }
    }

    if (nPeopleTotal == 0) {
        d->pose3dStatsLbl->setText(trackFilter >= 0 ? "No data for the selected track."
                                                    : "No people reconstructed in this result.");
        return;
    }

    const double avgError = errorCount > 0 ? errorSum / errorCount : 0.0;
    const double validPct = totalKpSum > 0 ? 100.0 * validKpSum / totalKpSum : 0.0;

    d->pose3dStatsLbl->setText(QString("%1 frame(s) with people  ·  %2 person-observation(s)  ·  "
                                       "%3% keypoints valid  ·  avg reprojection error %4 px")
                                   .arg(nFramesWithPeople)
                                   .arg(nPeopleTotal)
                                   .arg(validPct, 0, 'f', 0)
                                   .arg(avgError, 0, 'f', 1));
}

void AnalysisTabW::export_skeleton3d_csv() {
    const auto* info = d->current_session();
    if (!info || !d->currentSkeleton3D.is_valid()) {
        return;
    }

    const int trackFilter =
        d->trackCombo->currentIndex() >= 0 ? d->trackCombo->currentData().toInt() : -1;
    const QString suggested = info->path + "/skeleton3d.csv";
    const auto& names       = d->currentSkeleton3D.keypoint_names();

    export_csv(this, "Export 3D Pose", suggested, [&](QTextStream& ts) {
        // Raw AND smoothed columns, always both — matches this codebase's
        // established convention of never overwriting raw with smoothed
        // (item 16 pose kinematics, rPPG's own export_rppg_csv() bpm/
        // smoothed_bpm columns just above).
        ts << "tick,timestamp_ns,track_id,keypoint_name,"
              "x_mm,y_mm,z_mm,x_mm_smoothed,y_mm_smoothed,z_mm_smoothed,"
              "valid,reprojection_error_px\n";
        for (const auto& frame : d->currentSkeleton3D.frames()) {
            for (const auto& person : frame.people) {
                if (trackFilter >= 0 && person.trackId != trackFilter) {
                    continue;
                }
                for (int i = 0; i < person.keypoints.size(); ++i) {
                    const auto& kp       = person.keypoints[i];
                    const QString kpName = i < names.size() ? names[i] : QString("kp%1").arg(i);
                    ts << frame.tick << "," << frame.timestampNs << "," << person.trackId << ","
                       << kpName << ","
                       << (kp.valid ? QString::number(kp.positionRoom[0]) : QString()) << ","
                       << (kp.valid ? QString::number(kp.positionRoom[1]) : QString()) << ","
                       << (kp.valid ? QString::number(kp.positionRoom[2]) : QString()) << ","
                       << (kp.valid ? QString::number(kp.positionRoomSmoothed[0]) : QString())
                       << ","
                       << (kp.valid ? QString::number(kp.positionRoomSmoothed[1]) : QString())
                       << ","
                       << (kp.valid ? QString::number(kp.positionRoomSmoothed[2]) : QString())
                       << "," << (kp.valid ? "1" : "0") << ","
                       << (kp.valid ? QString::number(kp.reprojectionErrorPx) : QString()) << "\n";
                }
            }
        }
    });
}

// ── Dyad Analysis view ───────────────────────────────────────────────────
// (interpersonal metrics derived from the pose3d plugin's own already-loaded
// currentSkeleton3D — see src/analysis/dyadic_kinematics.hpp)

void AnalysisTabW::update_dyadic_view() {
    if (!d->dyadTrackACombo->isEnabled() || d->dyadTrackACombo->count() < 2) {
        d->currentDyad = DyadicKinematicsSeries();
        d->dyadStatsLbl->setText("Only 1 tracked person in this result — dyad analysis needs 2.");
        d->skeleton3dRoomView->set_dyad_tracks(-1, -1);
        d->chart->set_single_series({}, QString());
        d->chart->set_title("Dyad Analysis — needs 2 tracked people");
        return;
    }

    const int trackIdA = d->dyadTrackACombo->currentData().toInt();
    const int trackIdB = d->dyadTrackBCombo->currentData().toInt();

    // The two combos have no built-in mutual exclusion (either can be set
    // to the same track), and compute_dyadic_kinematics() intentionally
    // returns an empty series for that case (it isn't a real pair) — catch
    // it explicitly here with an accurate message instead of letting it
    // fall through to the generic "no overlapping data" text below, which
    // would misattribute the real cause.
    if (trackIdA == trackIdB) {
        d->currentDyad = DyadicKinematicsSeries();
        d->dyadStatsLbl->setText("Select two different people to compare.");
        d->skeleton3dRoomView->set_dyad_tracks(-1, -1);
        d->chart->set_single_series({}, QString());
        d->chart->set_title("Dyad Analysis — select two different tracks");
        return;
    }

    d->currentDyad = compute_dyadic_kinematics(d->currentSkeleton3D, trackIdA, trackIdB,
                                               d->dyadWindowSpin->value());
    d->skeleton3dRoomView->set_dyad_tracks(trackIdA, trackIdB);

    const auto& stats = d->currentDyad.stats;
    if (std::isnan(stats.meanDistanceMm)) {
        d->dyadStatsLbl->setText("No overlapping tracked data for this pair.");
    } else {
        const QString facingText    = std::isnan(stats.meanFacingCosine)
                                          ? "n/a"
                                          : QString::number(stats.meanFacingCosine, 'f', 2);
        const QString congruentText = std::isnan(stats.meanCongruentMotionCorr)
                                          ? "n/a"
                                          : QString::number(stats.meanCongruentMotionCorr, 'f', 2);
        d->dyadStatsLbl->setText(
            QString("dist %1mm (min %2, max %3)  ·  facing %4  ·  congruent motion %5  ·  "
                    "%6% ticks both present")
                .arg(stats.meanDistanceMm, 0, 'f', 0)
                .arg(stats.minDistanceMm, 0, 'f', 0)
                .arg(stats.maxDistanceMm, 0, 'f', 0)
                .arg(facingText, congruentText)
                .arg(stats.pctTicksBothPresent * 100.0, 0, 'f', 0));
    }

    const int64_t t0 = d->currentSkeleton3D.frames().isEmpty()
                           ? 0
                           : d->currentSkeleton3D.frames().first().timestampNs;
    const double minDurationMs =
        d->currentSkeleton3D.frames().isEmpty()
            ? 0.0
            : (d->currentSkeleton3D.frames().last().timestampNs - t0) / 1e6;

    const QString metric = d->dyadMetricCombo->currentData().toString();
    QString yAxisLabel;
    QVector<QPointF> points;
    points.reserve(d->currentDyad.samples.size());
    for (const auto& sample : d->currentDyad.samples) {
        double value = std::numeric_limits<double>::quiet_NaN();
        if (metric == "distance") {
            value      = sample.distanceMm;
            yAxisLabel = "Distance (mm)";
        } else if (metric == "approach_rate") {
            value      = sample.approachRateMmPerS;
            yAxisLabel = "Approach rate (mm/s, negative = closing)";
        } else if (metric == "facing") {
            value      = sample.facingCosine;
            yAxisLabel = "Facingness (cosine)";
        } else { // "congruent_motion"
            value      = sample.congruentMotionCorr;
            yAxisLabel = "Congruent motion (r)";
        }
        if (std::isnan(value)) {
            continue;
        }
        points.append(QPointF((sample.timestampNs - t0) / 1e6, value));
    }
    d->chart->set_single_series(points, yAxisLabel, QString(), minDurationMs);
    d->chart->set_title(QString("Dyad: Track %1 ↔ Track %2 — %3")
                            .arg(trackIdA)
                            .arg(trackIdB)
                            .arg(d->dyadMetricCombo->currentText()));
    d->chart->set_playhead_ms(d->player->position_ms());
}

void AnalysisTabW::export_dyad_csv() {
    const auto* info = d->current_session();
    if (!info || d->currentDyad.samples.isEmpty()) {
        return;
    }

    const int trackIdA      = d->dyadTrackACombo->currentData().toInt();
    const int trackIdB      = d->dyadTrackBCombo->currentData().toInt();
    const QString suggested = info->path + QString("/dyad_%1_%2.csv").arg(trackIdA).arg(trackIdB);

    export_csv(this, "Export Dyad Analysis", suggested, [&](QTextStream& ts) {
        ts << "# facing_cosine: -1 ~ oriented oppositely (commonly face-to-face in a two-person "
              "interaction, but also consistent with standing back-to-back facing opposite "
              "directions - position-independent by construction); +1 ~ oriented the same way\n";
        ts << "tick,timestamp_ns,distance_mm,approach_rate_mm_s,facing_cosine,"
              "congruent_motion_corr\n";
        const auto& frames = d->currentSkeleton3D.frames();
        for (int i = 0; i < d->currentDyad.samples.size(); ++i) {
            const auto& s = d->currentDyad.samples[i];
            // samples[i] corresponds 1:1, in order, to frames()[i] — see
            // compute_dyadic_kinematics()'s own doc comment.
            const int64_t tick      = i < frames.size() ? frames[i].tick : i;
            const auto num_or_blank = [](double v) {
                return std::isnan(v) ? QString() : QString::number(v);
            };
            ts << tick << "," << s.timestampNs << "," << num_or_blank(s.distanceMm) << ","
               << num_or_blank(s.approachRateMmPerS) << "," << num_or_blank(s.facingCosine) << ","
               << num_or_blank(s.congruentMotionCorr) << "\n";
        }
    });
}

void AnalysisTabW::update_trigger_sync_view() {
    const auto& m   = d->currentTriggerFrameMap;
    const int nCams = m.camera_count();

    d->triggerSyncTable->setColumnCount(7 + 2 * nCams);
    QStringList headers = {"Row", "Elapsed (ms)", "Wall clock", "Source", "Label", "Code", "Value"};
    for (int c = 0; c < nCams; ++c) {
        headers << QString("Cam%1 Frame").arg(c) << QString("Cam%1 Δms").arg(c);
    }
    d->triggerSyncTable->setHorizontalHeaderLabels(headers);

    const auto& rows = m.rows();
    d->triggerSyncTable->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];

        auto set_cell = [this, i](int col, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            d->triggerSyncTable->setItem(i, col, item);
        };

        set_cell(0, QString::number(row.rowIndex));
        set_cell(1, QString::number(row.elapsedMs, 'f', 1));
        set_cell(2, row.wallClock);
        set_cell(3, row.source);
        set_cell(4, row.label);
        set_cell(5, QString::number(row.code));
        set_cell(6, QString::number(row.value, 'f', 3));

        for (int c = 0; c < row.frames.size(); ++c) {
            const auto& hit = row.frames[c];
            set_cell(7 + 2 * c, hit.frameId >= 0 ? QString::number(hit.frameId) : QString("—"));

            // Flag large timing errors visibly rather than presenting an
            // extrapolated clamp (a trigger before the first frame or after
            // the last) as if it were a true nearest neighbor — 100ms is a
            // generous bound above any realistic single-camera nearest-frame
            // delta at typical 15-30fps (33-66ms frame periods).
            auto* deltaItem = new QTableWidgetItem(QString::number(hit.deltaMs, 'f', 1));
            deltaItem->setFlags(deltaItem->flags() & ~Qt::ItemIsEditable);
            if (hit.frameId >= 0 && std::abs(hit.deltaMs) > 100.0) {
                deltaItem->setForeground(QColor("#cc4444"));
            }
            d->triggerSyncTable->setItem(i, 8 + 2 * c, deltaItem);
        }
    }

    if (!m.is_valid()) {
        d->triggerSyncStatsLbl->clear();
        return;
    }
    d->triggerSyncStatsLbl->setText(
        QString("%1 trigger(s) resolved across %2 camera(s).").arg(m.trigger_count()).arg(nCams));
}

void AnalysisTabW::export_trigger_sync_csv() {
    const auto* info = d->current_session();
    if (!info || !d->currentTriggerFrameMap.is_valid()) {
        return;
    }

    const QString dst =
        QFileDialog::getSaveFileName(this, "Export Trigger/Frame Sync",
                                     info->path + "/trigger_frame_map.csv", "CSV files (*.csv)");
    if (dst.isEmpty()) {
        return;
    }
    d->currentTriggerFrameMap.export_csv(dst);
}

void AnalysisTabW::update_rppg_view() {
    const auto& result = d->currentRppgResult;
    if (!result.is_valid() || result.windows().isEmpty()) {
        d->chart->set_single_series({}, "BPM");
        d->chart->set_title("No analysis yet");
        d->rppgStatsLbl->clear();
        d->rppgQualityBadge->setText("—");
        d->rppgQualityBadge->setStyleSheet(badge_stylesheet(RmsQuality::Poor));
        d->chart->set_playhead_ms(d->player->position_ms());
        return;
    }

    const bool showSmoothed = d->rppgShowSmoothedCheck->isChecked();
    QVector<QPointF> points;
    points.reserve(result.windows().size());
    for (const auto& win : result.windows()) {
        // startMs/endMs are already video-relative (see run_rppg.py's
        // timestamp handling — no separate t0 subtraction needed, unlike
        // Pose's/Expression's charts). Plotted at the window's midpoint,
        // the conventional way to represent a sliding-window estimate.
        const double bpm = showSmoothed ? win.smoothedBpm : win.bpm;
        if (std::isnan(bpm)) {
            continue;
        } // no reliable estimate this window — skip, don't fabricate
        points.append(QPointF((win.startMs + win.endMs) / 2.0, bpm));
    }

    // Floors the axis at the last window's own end time (regardless of
    // whether every window had a usable estimate), matching
    // update_expression_view()'s identical reasoning for why the axis must
    // span the full analyzed range, not just the plotted points.
    const double lastAnalyzedMs = static_cast<double>(result.windows().last().endMs);
    d->chart->set_single_series(points, "BPM", showSmoothed ? "Smoothed BPM" : "Raw BPM",
                                lastAnalyzedMs);
    d->chart->set_title(QString("Heart rate — %1").arg(result.backend().toUpper()));
    d->chart->set_playhead_ms(d->player->position_ms());

    QStringList parts;
    parts << QString("backend %1").arg(result.backend());
    if (result.mean_bpm()) {
        parts << QString("mean %1 bpm").arg(*result.mean_bpm(), 0, 'f', 1);
    }
    if (result.min_bpm() && result.max_bpm()) {
        parts << QString("range %1–%2 bpm")
                     .arg(*result.min_bpm(), 0, 'f', 1)
                     .arg(*result.max_bpm(), 0, 'f', 1);
    }
    parts << QString("%1% windows usable").arg(result.pct_windows_good() * 100.0, 0, 'f', 0);
    d->rppgStatsLbl->setText(parts.join("  ·  "));

    // Aggregate quality badge — mean SNR across windows with a usable
    // estimate, mean valid-frame-fraction across every window (including
    // no-estimate ones, since a low detection rate is itself the signal
    // rppg_quality_for()'s validFrameFraction gate is meant to catch).
    // Was previously computed nowhere despite rppg_quality_for() existing
    // specifically to serve this badge — see its own doc comment.
    double snrSum = 0.0, fracSum = 0.0;
    int snrCount = 0;
    for (const auto& win : result.windows()) {
        if (!std::isnan(win.snrDb)) {
            snrSum += win.snrDb;
            ++snrCount;
        }
        fracSum += win.validFrameFraction;
    }
    const double meanSnr =
        snrCount > 0 ? snrSum / snrCount : std::numeric_limits<double>::quiet_NaN();
    const double meanFrac    = fracSum / result.windows().size();
    const RmsQuality quality = rppg_quality_for(meanSnr, meanFrac);
    d->rppgQualityBadge->setStyleSheet(badge_stylesheet(quality));
    QString tierName;
    switch (quality) {
        case RmsQuality::Excellent:
            tierName = "Excellent";
            break;
        case RmsQuality::Good:
            tierName = "Good";
            break;
        case RmsQuality::Acceptable:
            tierName = "Acceptable";
            break;
        case RmsQuality::Poor:
            tierName = "Poor";
            break;
    }
    d->rppgQualityBadge->setText(
        std::isnan(meanSnr) ? QString("Quality: %1").arg(tierName)
                            : QString("Quality: %1 (%2 dB)").arg(tierName).arg(meanSnr, 0, 'f', 1));
}

void AnalysisTabW::export_rppg_csv() {
    const auto* info = d->current_session();
    if (!info || !d->currentRppgResult.is_valid()) {
        return;
    }

    const QString suggested = info->path + "/rppg_" + d->currentRppgResult.backend() + ".csv";

    export_csv(this, "Export Heart Rate", suggested, [&](QTextStream& ts) {
        ts << "# EXPERIMENTAL research estimate only — not a medical device, "
              "not clinically validated. See mosaic-rppg-v1 schema docs.\n";
        ts << "# backend: " << d->currentRppgResult.backend() << "\n";
        ts << "start_ms,end_ms,bpm,smoothed_bpm,snr_db,valid_frame_fraction\n";
        for (const auto& win : d->currentRppgResult.windows()) {
            ts << win.startMs << "," << win.endMs << ","
               << (std::isnan(win.bpm) ? QString() : QString::number(win.bpm)) << ","
               << (std::isnan(win.smoothedBpm) ? QString() : QString::number(win.smoothedBpm))
               << "," << (std::isnan(win.snrDb) ? QString() : QString::number(win.snrDb)) << ","
               << win.validFrameFraction << "\n";
        }
    });
}

void AnalysisTabW::update_gaze2d_view() {
    const auto& result = d->currentGaze2dResult;
    if (!result.is_valid() || result.frames().isEmpty()) {
        d->chart->set_single_series({}, "Gaze");
        d->chart->set_title("No analysis yet");
        d->gaze2dStatsLbl->clear();
        d->chart->set_playhead_ms(d->player->position_ms());
        return;
    }

    const QString metric = d->gaze2dMetricCombo->currentData().toString();

    // Gaze2dFrame::timestampMs is app-launch-relative (elapsed_ns from
    // timestamps_camN.csv), NOT video-relative — same convention
    // RppgFrame/RppgWindow use (see PoseOverlayPlayerW::Impl::
    // gaze2d_timestamp_estimate()'s doc comment). The chart's x-axis and
    // set_playhead_ms() below both operate in the PLAYER's video-relative
    // (0-based) timeline, so t0 must be subtracted here before plotting —
    // matching update_expression_view()'s identical t0-normalization,
    // rather than plotting the raw absolute timestamp (which would offset
    // every point, and the playhead line, by the recording's pre-analysis
    // elapsed time).
    const int64_t t0 = result.frames().first().timestampMs;

    QVector<QPointF> points;
    points.reserve(result.frames().size());
    for (const auto& frame : result.frames()) {
        if (!frame.faceDetected) {
            continue; // no detection this frame — skip, don't fabricate
        }
        double value = 0.0;
        if (metric == "dx") {
            value = frame.gazeDx;
        } else if (metric == "dy") {
            value = frame.gazeDy;
        } else {
            value = std::hypot(frame.gazeDx, frame.gazeDy);
        }
        points.append(QPointF(static_cast<double>(frame.timestampMs - t0), value));
    }

    // Floors the axis at the last analyzed frame's own (t0-relative)
    // timestamp (regardless of whether every frame had a detected face),
    // matching update_expression_view()'s identical reasoning for why the
    // axis must span the full analyzed range, not just the plotted points.
    const double lastAnalyzedMs = static_cast<double>(result.frames().last().timestampMs - t0);
    d->chart->set_single_series(points, "Gaze " + metric, QString(), lastAnalyzedMs);
    d->chart->set_title("2D Gaze — " + metric);
    d->chart->set_playhead_ms(d->player->position_ms());

    QStringList parts;
    parts << QString("%1% frames with face").arg(result.pct_frames_with_face() * 100.0, 0, 'f', 0);
    if (result.mean_gaze_dx() && result.mean_gaze_dy()) {
        parts << QString("mean dx %1  dy %2")
                     .arg(*result.mean_gaze_dx(), 0, 'f', 2)
                     .arg(*result.mean_gaze_dy(), 0, 'f', 2);
    }
    if (result.pct_on_target()) {
        parts << QString("%1% on target").arg(*result.pct_on_target() * 100.0, 0, 'f', 0);
    }
    d->gaze2dStatsLbl->setText(parts.join("  ·  "));
}

void AnalysisTabW::export_gaze2d_csv() {
    const auto* info = d->current_session();
    if (!info || !d->currentGaze2dResult.is_valid()) {
        return;
    }

    const QString suggested = info->path + "/gaze2d.csv";

    export_csv(this, "Export 2D Gaze", suggested, [&](QTextStream& ts) {
        ts << "frame_index,timestamp_ms,face_detected,face_box_x,face_box_y,face_box_w,"
              "face_box_h,gaze_dx,gaze_dy\n";
        for (const auto& frame : d->currentGaze2dResult.frames()) {
            ts << frame.frameIndex << "," << frame.timestampMs << "," << frame.faceDetected << ","
               << frame.faceBoxPx.x() << "," << frame.faceBoxPx.y() << ","
               << frame.faceBoxPx.width() << "," << frame.faceBoxPx.height() << ","
               << (frame.faceDetected ? QString::number(frame.gazeDx) : QString()) << ","
               << (frame.faceDetected ? QString::number(frame.gazeDy) : QString()) << "\n";
        }
    });
}

void AnalysisTabW::run_analysis() {
    if (d->currentSessionPath.isEmpty()) {
        return;
    }

    const QString plugin = d->pluginCombo->currentData().toString();

    if (plugin == "trigger_sync") {
        // Computed synchronously in C++ — no AnalysisManager subprocess job,
        // no progress bar, no analysis_started/finished signal dance; the
        // whole operation completes within this call (see
        // TriggerFrameMap's own doc comment for why this plugin is the one
        // exception to every other plugin's Python-subprocess pattern —
        // it's fast, deterministic CSV-to-CSV arithmetic with no ML
        // dependency, the same shape as SyncManifest's own synchronous
        // generate()).
        d->currentTriggerFrameMap = TriggerFrameMap::generate(d->currentSessionPath);
        if (d->currentTriggerFrameMap.is_valid()) {
            d->currentTriggerFrameMap.save(d->currentSessionPath);
            d->statusLbl->setText(QString("Done — %1 trigger(s) resolved across %2 camera(s).")
                                      .arg(d->currentTriggerFrameMap.trigger_count())
                                      .arg(d->currentTriggerFrameMap.camera_count()));
            d->statusLbl->setStyleSheet("color:#44cc66; font-size:15px; font-weight:600;");
            update_trigger_sync_view();
        } else {
            d->statusLbl->setText(
                "Error: no trigger.csv with elapsed_ns, or no timestamps_camN.csv found.");
            d->statusLbl->setStyleSheet("color:#cc4444; font-size:15px; font-weight:600;");
        }
        return;
    }

    if (plugin == "diarize") {
        const int minSpeakers = d->minSpeakersSpin->value();
        const int maxSpeakers = d->maxSpeakersSpin->value();
        if (minSpeakers > 0 && maxSpeakers > 0 && minSpeakers > maxSpeakers) {
            d->statusLbl->setText("Error: Min speakers can't exceed Max speakers.");
            d->statusLbl->setStyleSheet("color:#cc4444; font-size:15px; font-weight:600;");
            return;
        }
    }

    if (plugin == "pose3d") {
        // Validation-only pre-check, no dispatch (mirrors diarize's
        // min>max-speakers guard above) — run_pose3d.py reads .pose.json
        // sidecars rather than running its own inference, so launching it
        // against a session with no Pose output would just fail deep
        // inside the script with a less actionable error.
        const auto* info = d->current_session();
        int nWithPose    = 0;
        if (info) {
            for (const auto& videoRel : info->videoFiles) {
                if (QFileInfo::exists(info->path + "/" + pose_json_path_for(videoRel))) {
                    ++nWithPose;
                }
            }
        }
        if (nWithPose < 2) {
            d->statusLbl->setText(
                "Error: run the Pose plugin on at least 2 cameras in this session first.");
            d->statusLbl->setStyleSheet("color:#cc4444; font-size:15px; font-weight:600;");
            return;
        }
    }

    d->jobPlugin = plugin;
    if (plugin == "pose") {
        d->analysisMgr->set_model(d->modelCombo->currentData().toString());
        d->analysisMgr->set_frame_skip(d->skipSpin->value());
        d->analysisMgr->analyze_session(d->currentSessionPath);
    } else if (plugin == "diarize") {
        d->analysisMgr->run_diarization(
            d->currentSessionPath, d->whisperModelCombo->currentData().toString(),
            d->languageCombo->currentData().toString(), d->settings.analysis.hfToken,
            d->minSpeakersSpin->value(), d->maxSpeakersSpin->value(),
            d->skipDiarizationCheck->isChecked());
    } else if (plugin == "face_mask") {
        d->analysisMgr->run_face_mask(
            d->currentSessionPath, d->backendCombo->currentData().toString(),
            d->styleCombo->currentData().toString(), d->faceSkipSpin->value());
    } else if (plugin == "expression") {
        d->analysisMgr->run_expression_analysis(
            d->currentSessionPath, d->expressionBackendCombo->currentData().toString(),
            d->maxFacesSpin->value(), d->minConfidenceSpin->value(), d->exprSkipSpin->value());
    } else if (plugin == "gaze_fusion") {
        d->analysisMgr->run_gaze_fusion(d->currentSessionPath, d->minCamerasSpin->value(),
                                        d->gazeMinConfidenceSpin->value(),
                                        d->gazeSkipSpin->value());
    } else if (plugin == "pose3d") {
        d->analysisMgr->run_pose3d_reconstruction(
            d->currentSessionPath, d->pose3dMinCamerasSpin->value(),
            d->maxReprojectionErrorSpin->value(), d->pose3dSkipSpin->value(),
            d->pose3dSmoothingWindowSpin->value());
    } else if (plugin == "rppg") {
        d->analysisMgr->run_rppg_analysis(d->currentSessionPath,
                                          d->rppgBackendCombo->currentData().toString(),
                                          d->rppgWindowSecSpin->value(), d->rppgHopSecSpin->value(),
                                          d->rppgSmoothingSpin->value());
    } else if (plugin == "gaze2d") {
        d->analysisMgr->run_gaze2d_analysis(
            d->currentSessionPath, d->gaze2dMinConfidenceSpin->value(), d->gaze2dSkipSpin->value());
    } else {
        // Defensive: pluginCombo only ever offers the ids handled above, but
        // a silent fallthrough here would otherwise launch face-masking with
        // whatever plugin's controls happen to be on screen.
        d->statusLbl->setText("Error: unknown plugin selected.");
        d->statusLbl->setStyleSheet("color:#cc4444; font-size:15px; font-weight:600;");
    }
}

void AnalysisTabW::open_output_folder() {
    const auto* info = d->current_session();
    if (!info) {
        return;
    }
    const QString folder = is_pose_depth_selected() ? "depth" : "anonymized";
    QDesktopServices::openUrl(QUrl::fromLocalFile(info->path + "/" + folder));
}

} // namespace mosaic
