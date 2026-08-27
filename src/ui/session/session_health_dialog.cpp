#include "ui/session/session_health_dialog.hpp"

#include <QDesktopServices>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
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
    const QStringList headers = {"CAMERA",       "GRABBED",    "ENCODED",
                                 "DROPPED",      "INCOMPLETE", "FPS (ACHV/CFG)",
                                 "MISSED TRIG.", "SYNC",       "STATUS"};
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
        grid->addWidget(
            value_label(raw.name.isEmpty() ? QString("Camera %1").arg(raw.index) : raw.name), row,
            col++);
        grid->addWidget(value_label(QString::number(raw.framesGrabbed)), row, col++);
        grid->addWidget(value_label(QString::number(raw.framesEncoded)), row, col++);
        grid->addWidget(value_label(QString::number(raw.framesDropped)), row, col++);
        grid->addWidget(value_label(QString::number(raw.incompleteFrames)), row, col++);
        grid->addWidget(value_label(fps_pair(raw.configuredFps, raw.achievableFps)), row, col++);
        grid->addWidget(value_label(opt_int(entry.missedTriggerFrames)), row, col++);
        grid->addWidget(value_label(opt_pct(raw.syncCoveragePct)), row, col++);
        auto* pill = new QLabel(entry.quality == RmsQuality::Excellent    ? "Excellent"
                                : entry.quality == RmsQuality::Good       ? "Good"
                                : entry.quality == RmsQuality::Acceptable ? "Acceptable"
                                                                          : "Poor");
        pill->setStyleSheet(badge_stylesheet(entry.quality));
        pill->setAlignment(Qt::AlignCenter);
        grid->addWidget(pill, row, col++);
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

    // ── Footer ───────────────────────────────────────────────────────────
    auto* footerRow     = new QHBoxLayout;
    auto* openFolderBtn = new QPushButton("Open session folder");
    connect(openFolderBtn, &QPushButton::clicked, this,
            [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(m_report.sessionPath)); });
    footerRow->addWidget(openFolderBtn);
    footerRow->addStretch();

    auto* closeBtn = new QPushButton("Close");
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
    footerRow->addWidget(closeBtn);
    lay->addLayout(footerRow);
}

} // namespace mosaic
