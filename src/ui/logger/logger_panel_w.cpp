#include "ui/logger/logger_panel_w.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QTextEdit>
#include <QVBoxLayout>

namespace mosaic {

static const char* k_level_labels[] = {
    "Trace", "Debug", "Info", "Warning", "Error", "Critical"
};

static const char* k_level_colours[] = {
    "#555575",  // Trace
    "#555575",  // Debug
    "#c8c8e0",  // Info
    "#ddaa44",  // Warning
    "#dd4444",  // Error
    "#ff2222",  // Critical
};

struct LoggerPanelW::Impl {
    QTextEdit*  view        = nullptr;
    QComboBox*  levelFilter = nullptr;
    QCheckBox*  autoScroll  = nullptr;
    int         maxLines    = 3000;
    int         lineCount   = 0;
    // Defaults to Error, not Trace/Warning — routine INFO/WARNING-level
    // activity (e.g. the Calibration tab's per-frame feed/detection
    // logging, expected frame-rate-adjustment warnings) otherwise floods
    // this panel during normal use, burying the failures that actually
    // need attention. Error (not Critical) is the default rather than
    // Critical because Critical is reserved for "the application cannot
    // continue" — genuine, non-fatal failures (a camera failing to open, a
    // subprocess crashing) are logged at Error and would otherwise never
    // show up by default. Still fully adjustable via the Show combo below,
    // which stays in sync with this default.
    int         minLevel    = static_cast<int>(LogLevel::Error);
};

LoggerPanelW::LoggerPanelW(QWidget* parent)
    : QWidget(parent), d(std::make_unique<Impl>())
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);

    build_toolbar(lay);

    d->view = new QTextEdit;
    d->view->setReadOnly(true);
    d->view->setUndoRedoEnabled(false);
    d->view->setLineWrapMode(QTextEdit::NoWrap);
    d->view->setFont(QFont("Courier New, Courier, monospace", 10));
    d->view->setStyleSheet(
        "QTextEdit { background: #070710; border: 1px solid #1a1a35; border-radius: 4px; }");
    lay->addWidget(d->view, 1);

    // Connect to Logger — must use QueuedConnection so cross-thread signals are safe.
    connect(&Logger::instance(), &Logger::entry_added,
            this,                &LoggerPanelW::on_entry_added,
            Qt::QueuedConnection);
}

LoggerPanelW::~LoggerPanelW() = default;

void LoggerPanelW::set_max_lines(int max) { d->maxLines = max; }

// ── Toolbar ────────────────────────────────────────────────────────────────

void LoggerPanelW::build_toolbar(QVBoxLayout* parent) {
    auto* row = new QHBoxLayout;
    row->setContentsMargins(4, 4, 4, 0);
    row->setSpacing(8);

    // Level filter
    auto* filterLbl = new QLabel("Show:");
    filterLbl->setProperty("role", "muted");
    row->addWidget(filterLbl);

    d->levelFilter = new QComboBox;
    for (const auto* lbl : k_level_labels) { d->levelFilter->addItem(lbl); }
    d->levelFilter->setCurrentIndex(static_cast<int>(LogLevel::Error));
    d->levelFilter->setFixedWidth(90);
    connect(d->levelFilter, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int idx) { d->minLevel = idx; });
    row->addWidget(d->levelFilter);

    row->addStretch();

    // Auto-scroll toggle
    d->autoScroll = new QCheckBox("Auto-scroll");
    d->autoScroll->setChecked(true);
    row->addWidget(d->autoScroll);

    // Clear button
    auto* clearBtn = new QPushButton("Clear");
    clearBtn->setFixedWidth(60);
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        d->view->clear();
        d->lineCount = 0;
    });
    row->addWidget(clearBtn);

    auto* rowWidget = new QWidget;
    rowWidget->setLayout(row);
    parent->addWidget(rowWidget);
}

// ── Entry rendering ────────────────────────────────────────────────────────

void LoggerPanelW::on_entry_added(int level, QString timestamp,
                                    QString location, QString message) {
    if (level < d->minLevel) { return; }
    append_entry(level, timestamp, location, message);
}

void LoggerPanelW::append_entry(int level, const QString& timestamp,
                                  const QString& location, const QString& message) {
    const int   clampedLevel = std::clamp(level, 0, 5);
    const char* colour       = k_level_colours[clampedLevel];
    const char* lvlLabel     = k_level_labels[clampedLevel];

    // Trim oldest lines when we exceed the cap.
    if (d->lineCount >= d->maxLines) {
        QTextCursor cursor(d->view->document());
        cursor.movePosition(QTextCursor::Start);
        cursor.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor,
                            d->maxLines / 10); // remove 10% at a time
        cursor.removeSelectedText();
        d->lineCount -= d->maxLines / 10;
    }

    // Build an HTML line — rich enough to colour-code without heavy DOM work.
    const QString html = QString(
        "<span style='color:#404060'>%1</span> "
        "<span style='color:%2;font-weight:%3'>%4</span> "
        "<span style='color:#404060;font-size:9px'>%5</span> "
        "<span style='color:%2'>%6</span>")
        .arg(timestamp.toHtmlEscaped())
        .arg(colour)
        .arg(level >= static_cast<int>(LogLevel::Critical) ? "bold" : "normal")
        .arg(QString(lvlLabel).toHtmlEscaped())
        .arg(location.toHtmlEscaped())
        .arg(message.toHtmlEscaped());

    d->view->append(html);
    ++d->lineCount;

    if (d->autoScroll->isChecked()) {
        d->view->verticalScrollBar()->setValue(
            d->view->verticalScrollBar()->maximum());
    }
}

} // namespace mosaic
