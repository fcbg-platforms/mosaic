#include "ui/help_dialog.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace mosaic {

namespace {

QLabel* section_header(const QString& text) {
    auto* lbl = new QLabel(text);
    lbl->setStyleSheet(
        "color: #6666aa; font-size: 11px; font-weight: bold; "
        "letter-spacing: 1.5px; margin-top: 4px;");
    return lbl;
}

QFrame* divider() {
    auto* line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setStyleSheet("QFrame { background: #202042; border: none; max-height: 1px; }");
    return line;
}

} // namespace

HelpDialog::HelpDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("About MOSAIC");
    setMinimumWidth(480);
    setStyleSheet(
        "QDialog { background: #0d0d1e; }"
        "QLabel { color: #c8c8e0; }"
        "QPushButton[role=\"link\"] {"
        "  background: transparent; border: 1px solid #303060; border-radius: 5px;"
        "  padding: 7px 16px; color: #8888dd; font-size: 12px; text-align: left; }"
        "QPushButton[role=\"link\"]:hover { border-color: #5050aa; color: #aaaaff; background: "
        "#14142c; }");

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(28, 26, 28, 22);
    lay->setSpacing(4);

    // ── Header ───────────────────────────────────────────────────────────
    auto* titleRow = new QHBoxLayout;
    auto* title    = new QLabel("MOSAIC");
    title->setStyleSheet(
        "color: #c8c8ff; font-size: 24px; font-weight: bold; letter-spacing: 2px;");
    titleRow->addWidget(title);

    auto* version = new QLabel(QString("v%1").arg(QCoreApplication::applicationVersion()));
    version->setStyleSheet("color: #5a5a8a; font-size: 13px; padding-top: 8px;");
    titleRow->addWidget(version);
    titleRow->addStretch();
    lay->addLayout(titleRow);

    auto* tagline =
        new QLabel("Multi-camera Observatory for Social &amp; Activity Interaction Capture");
    tagline->setStyleSheet("color: #8888aa; font-size: 13px;");
    tagline->setWordWrap(true);
    lay->addWidget(tagline);
    lay->addSpacing(6);
    lay->addWidget(divider());
    lay->addSpacing(10);

    // ── About ────────────────────────────────────────────────────────────
    auto* about = new QLabel(
        "A synchronized multi-camera + audio recording suite for research labs, built "
        "around Basler GigE cameras, with live pose/gaze preview, hardware-triggered "
        "sync, and a full post-recording analysis pipeline.");
    about->setWordWrap(true);
    about->setStyleSheet("font-size: 13px; color: #b0b0d0;");
    lay->addWidget(about);
    lay->addSpacing(12);

    // ── Key capabilities ─────────────────────────────────────────────────
    lay->addWidget(section_header("KEY CAPABILITIES"));
    lay->addSpacing(4);
    auto* caps = new QLabel(
        "•  Synchronized multi-camera GigE recording with audio<br>"
        "•  Live pose &amp; gaze preview, and a real-time analytics dashboard<br>"
        "•  Post-hoc analysis: pose, 3D reconstruction, gaze fusion, facial expression, "
        "face masking, speaker diarization<br>"
        "•  Hardware-triggered camera sync and EEG/parallel-port trigger alignment<br>"
        "•  Per-group research profiles with isolated settings and calibration");
    caps->setWordWrap(true);
    caps->setStyleSheet("font-size: 12px; color: #a0a0c0; line-height: 150%;");
    lay->addWidget(caps);
    lay->addSpacing(12);

    // ── Keyboard shortcuts ───────────────────────────────────────────────
    lay->addWidget(section_header("KEYBOARD SHORTCUTS"));
    lay->addSpacing(4);
    auto* shortcuts = new QGridLayout;
    shortcuts->setHorizontalSpacing(18);
    shortcuts->setVerticalSpacing(4);
    const std::pair<const char*, const char*> kShortcuts[] = {
        {"Ctrl+R", "Start recording"},
        {"Ctrl+.", "Stop recording"},
        {"Ctrl+Shift+O", "Open recordings folder"},
        {"Ctrl+B", "Browse sessions"},
        {"Ctrl+Shift+P", "Switch profile"},
        {"Ctrl+L", "Toggle log panel"},
        {"Ctrl+Q", "Quit"},
    };
    int row = 0;
    for (const auto& [keys, desc] : kShortcuts) {
        auto* keyLbl = new QLabel(keys);
        keyLbl->setStyleSheet(
            "font-family: Consolas, monospace; font-size: 11px; color: #ddaa44; "
            "background: #1a1a35; border-radius: 3px; padding: 2px 6px;");
        auto* descLbl = new QLabel(desc);
        descLbl->setStyleSheet("font-size: 12px; color: #a0a0c0;");
        shortcuts->addWidget(keyLbl, row, 0, Qt::AlignLeft);
        shortcuts->addWidget(descLbl, row, 1, Qt::AlignLeft);
        ++row;
    }
    shortcuts->setColumnStretch(1, 1);
    lay->addLayout(shortcuts);
    lay->addSpacing(14);
    lay->addWidget(divider());
    lay->addSpacing(10);

    // ── Links ────────────────────────────────────────────────────────────
    auto* linkRow = new QHBoxLayout;
    linkRow->setSpacing(8);

    auto* docsBtn = new QPushButton("📘  Documentation");
    docsBtn->setProperty("role", "link");
    connect(docsBtn, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl("https://github.com/fcbg-platforms/mosaic/tree/main/docs"));
    });
    linkRow->addWidget(docsBtn);

    auto* repoBtn = new QPushButton("🔗  GitHub repository");
    repoBtn->setProperty("role", "link");
    connect(repoBtn, &QPushButton::clicked, this,
            [] { QDesktopServices::openUrl(QUrl("https://github.com/fcbg-platforms/mosaic")); });
    linkRow->addWidget(repoBtn);

    auto* issueBtn = new QPushButton("🐛  Report an issue");
    issueBtn->setProperty("role", "link");
    connect(issueBtn, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl("https://github.com/fcbg-platforms/mosaic/issues/new"));
    });
    linkRow->addWidget(issueBtn);

    lay->addLayout(linkRow);
    lay->addSpacing(14);

    // ── Footer ───────────────────────────────────────────────────────────
    auto* footer = new QLabel(
        "Developed by <b style=\"color:#9999cc;\">Payam S. Shabestari</b> &middot; "
        "Fondation Campus Biotech Geneva (FCBG)");
    footer->setStyleSheet("color: #5a5a7a; font-size: 11px;");
    footer->setAlignment(Qt::AlignCenter);
    footer->setWordWrap(true);
    lay->addWidget(footer);

    auto* closeBtn = new QPushButton("Close");
    closeBtn->setStyleSheet(
        "QPushButton { background: #2828a0; border: 1px solid #4444cc; border-radius: 5px; "
        "  padding: 7px 28px; color: #c8c8ff; font-weight: bold; }"
        "QPushButton:hover { background: #3030b8; border-color: #6060ee; }"
        "QPushButton:pressed { background: #1c1c70; }");
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    auto* closeRow = new QHBoxLayout;
    closeRow->addStretch();
    closeRow->addWidget(closeBtn);
    closeRow->addStretch();
    lay->addSpacing(10);
    lay->addLayout(closeRow);
}

} // namespace mosaic
