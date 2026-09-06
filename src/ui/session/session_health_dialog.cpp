#include "ui/session/session_health_dialog.hpp"

#include <QDesktopServices>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>

#include "session/session_info.hpp"
#include "ui/calibration/badge_style.hpp"

namespace mosaic {

namespace {

QLabel* muted_label(const QString& text) {
    auto* lbl = new QLabel(text);
    lbl->setStyleSheet("color: #7070a0; font-size: 11px;");
    lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return lbl;
}

QLabel* value_label(const QString& text) {
    auto* lbl = new QLabel(text);
    lbl->setStyleSheet("color: #c8c8e0; font-size: 12px; font-family: monospace;");
    return lbl;
}

// "n/a" for an unset optional, matching PerformanceMonitorW/kinematics-style
// stats readouts elsewhere in this codebase — never a fabricated 0.
QString opt_int(const std::optional<int64_t>& v) {
    return v.has_value() ? QString::number(*v) : QStringLiteral("n/a");
}

QString opt_pct(const std::optional<double>& v) {
    return v.has_value() ? QString("%1%").arg(*v, 0, 'f', 1) : QStringLiteral("n/a");
}

// A camera that never opened has no counters at all — showing "0" would read
// as "recorded cleanly, captured nothing", which is a different and much less
// alarming statement than the truth.
QLabel* absent_label() {
    auto* lbl = new QLabel("not opened");
    lbl->setStyleSheet("color: #6a4a4a; font-size: 12px; font-family: monospace;");
    return lbl;
}

QString quality_text(RmsQuality q) {
    switch (q) {
        case RmsQuality::Excellent:
            return "Excellent";
        case RmsQuality::Good:
            return "Good";
        case RmsQuality::Acceptable:
            return "Acceptable";
        case RmsQuality::Poor:
            return "Poor";
    }
    return "Poor";
}

// Colour of the CAMERA cell's own text, standing in for the removed STATUS
// column so a bad row is still identifiable at a glance when several cameras
// are bad (the headline only ever names the worst one). Reuses style.hpp's
// existing warning/danger colours.
QString name_color_for(RmsQuality q) {
    switch (q) {
        case RmsQuality::Poor:
            return "#cc4444";
        case RmsQuality::Acceptable:
            return "#ddaa44";
        default:
            return "#c8c8e0";
    }
}

QString fps_pair(double configured, double achievable) {
    if (achievable < 0.0) {
        return QString("%1 (measuring…)").arg(configured, 0, 'f', 1);
    }
    return QString("%1 / %2").arg(achievable, 0, 'f', 1).arg(configured, 0, 'f', 1);
}

} // namespace

SessionHealthDialog::SessionHealthDialog(const SessionHealthReport& report, QWidget* parent)
    : QDialog(parent), m_report(report) {
    setWindowTitle("Session Health — " + report.sessionName);
    setMinimumWidth(620);
    setAttribute(Qt::WA_DeleteOnClose);
    setStyleSheet("QDialog { background: #0d0d1e; } QLabel { color: #c8c8e0; }");

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(24, 22, 24, 18);
    lay->setSpacing(10);

    // ── Header ───────────────────────────────────────────────────────────
    auto* headerRow = new QHBoxLayout;
    auto* title     = new QLabel(report.sessionName);
    title->setStyleSheet("color: #c8c8ff; font-size: 16px; font-weight: bold;");
    headerRow->addWidget(title);

    auto* duration = new QLabel(SessionInfo::ms_to_hms(report.durationMs));
    duration->setStyleSheet("color: #7070a0; font-size: 12px; padding-top: 3px;");
    headerRow->addWidget(duration);
    headerRow->addStretch();

    auto* overallPill = new QLabel(m_report.headline);
    overallPill->setStyleSheet(badge_stylesheet(report.overallQuality));
    headerRow->addWidget(overallPill);
    lay->addLayout(headerRow);

    auto* headerLine = new QFrame;
    headerLine->setFrameShape(QFrame::HLine);
    headerLine->setStyleSheet("QFrame { background: #1a1a35; border: none; max-height: 1px; }");
    lay->addWidget(headerLine);

    // ── Per-camera grid ──────────────────────────────────────────────────
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(4);

    const auto headerStyle =
        "color: #44446a; font-size: 9px; font-weight: bold; letter-spacing: 1px;";
    // No per-camera STATUS column — the overall-quality pill in the header row
    // carries the verdict, and each row's own camera name is tinted by its
    // quality (see name_color_for()) so a bad camera is still identifiable
    // without spending a whole column on it.
    const QStringList headers = {"CAMERA",     "GRABBED",        "ENCODED",      "DROPPED",
                                 "INCOMPLETE", "FPS (ACHV/CFG)", "MISSED TRIG.", "SYNC"};
    for (int col = 0; col < headers.size(); ++col) {
        auto* lbl = new QLabel(headers[col]);
        lbl->setStyleSheet(headerStyle);
        lbl->setAlignment(Qt::AlignCenter);
        grid->addWidget(lbl, 0, col);
    }

    int row = 1;
    for (const auto& entry : report.cameras) {
        const auto& raw = entry.raw;
        int col         = 0;

        // 1-based, matching build_session_health_report()'s own fallback and
        // CameraCardW's headers. Colour alone would be a poor carrier for the
        // verdict (Excellent and Good share a colour, and a screenshot or a
        // colour-blind reader loses it entirely), so a below-Good row also
        // says so in text.
        QString nameText = raw.name.isEmpty() ? QString("Camera %1").arg(raw.index + 1) : raw.name;
        if (entry.quality == RmsQuality::Acceptable || entry.quality == RmsQuality::Poor) {
            nameText += QString("  · %1").arg(quality_text(entry.quality));
        }
        auto* nameLbl = value_label(nameText);
        nameLbl->setStyleSheet(QString("color: %1; font-size: 12px; font-family: monospace;")
                                   .arg(name_color_for(entry.quality)));
        // The artifacts are 0-based (Camera 1 -> video_0.mp4) — but a camera
        // that never opened wrote none of them, so don't name files that
        // aren't there.
        nameLbl->setToolTip(
            raw.participated
                ? QString("%1 — files are named video_%2.mp4 / timestamps_cam%2.csv")
                      .arg(quality_text(entry.quality))
                      .arg(raw.index)
                : QString("%1 — this camera never opened, so it wrote no files for this session.")
                      .arg(quality_text(entry.quality)));
        grid->addWidget(nameLbl, row, col++);

        if (!raw.participated) {
            // Every remaining column is meaningless for a camera that never
            // opened — span them with one honest statement instead of seven
            // zeros.
            grid->addWidget(absent_label(), row, col, 1, headers.size() - 1);
            ++row;
            continue;
        }

        grid->addWidget(value_label(QString::number(raw.framesGrabbed)), row, col++);
        grid->addWidget(value_label(QString::number(raw.framesEncoded)), row, col++);
        grid->addWidget(value_label(QString::number(raw.framesDropped)), row, col++);
        grid->addWidget(value_label(QString::number(raw.incompleteFrames)), row, col++);
        grid->addWidget(value_label(fps_pair(raw.configuredFps, raw.achievableFps)), row, col++);
        grid->addWidget(value_label(opt_int(entry.missedTriggerFrames)), row, col++);
        grid->addWidget(value_label(opt_pct(raw.syncCoveragePct)), row, col++);
        ++row;
    }

    if (report.cameras.isEmpty()) {
        auto* none = new QLabel("No cameras recorded.");
        none->setStyleSheet("color: #333355; font-size: 11px; padding: 8px;");
        none->setAlignment(Qt::AlignCenter);
        grid->addWidget(none, 1, 0, 1, headers.size());
    }
    lay->addLayout(grid);
    lay->addStretch();

    // ── Session notes ────────────────────────────────────────────────────
    //
    // This dialog appears the moment recording stops, which is exactly when
    // the operator knows what actually happened — a note typed here is the
    // one most likely to be worth having. Prefilled with anything already
    // written before or during the recording, and saved on close rather than
    // behind a button, because a note lost to a forgotten Save is worse than
    // no note at all.
    auto* notesLabel = new QLabel("Notes");
    notesLabel->setStyleSheet("color: #7070a0; font-size: 11px;");
    lay->addWidget(notesLabel);

    m_notes = new QPlainTextEdit;
    m_notes->setPlaceholderText("Anything worth recording about this session…");
    m_notes->setFixedHeight(70);
    m_notes->setStyleSheet(
        "QPlainTextEdit { background: #09091a; color: #c8c8e0; "
        "border: 1px solid #1e1e40; border-radius: 4px; padding: 4px; }");
    {
        SessionInfo info;
        info.path = m_report.sessionPath;
        info.load_notes();
        m_notes->setPlainText(info.notes);
    }
    // setPlainText() marks the document modified; clear that so an untouched
    // box stays untouched. See save_notes().
    m_notes->document()->setModified(false);
    lay->addWidget(m_notes);

    // ── Footer ───────────────────────────────────────────────────────────
    auto* footerRow     = new QHBoxLayout;
    auto* openFolderBtn = new QPushButton("Open session folder");
    connect(openFolderBtn, &QPushButton::clicked, this,
            [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(m_report.sessionPath)); });
    footerRow->addWidget(openFolderBtn);
    footerRow->addStretch();

    auto* closeBtn = new QPushButton("Close");
    connect(closeBtn, &QPushButton::clicked, this, [this] {
        save_notes();
        close();
    });
    footerRow->addWidget(closeBtn);
    lay->addLayout(footerRow);
}

void SessionHealthDialog::save_notes() const {
    if (!m_notes) return;
    // Only write if the operator actually typed something. This dialog is
    // non-modal, so it can sit open while the same session's notes are edited
    // elsewhere (the Session Browser, or a second operator on a shared
    // directory); saving an untouched snapshot on close would revert that
    // edit, and save_notes() deletes notes.txt outright when the text is
    // empty — so an untouched empty box would destroy a note written in the
    // meantime.
    if (!m_notes->document()->isModified()) return;

    SessionInfo info;
    info.path  = m_report.sessionPath;
    info.notes = m_notes->toPlainText();
    info.save_notes();
}

// Also covers the window's X and Esc, not just the Close button — the dialog
// is non-modal and WA_DeleteOnClose, so an unsaved note would simply vanish.
void SessionHealthDialog::closeEvent(QCloseEvent* event) {
    save_notes();
    QDialog::closeEvent(event);
}

} // namespace mosaic
