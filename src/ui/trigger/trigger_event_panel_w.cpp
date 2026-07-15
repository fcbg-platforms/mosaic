#include "ui/trigger/trigger_event_panel_w.hpp"
#include "trigger/trigger_manager.hpp"
#include "utils/timestamp.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>

namespace mosaic {

// Column indices
static constexpr int kColTime    = 0;
static constexpr int kColElapsed = 1;
static constexpr int kColSource  = 2;
static constexpr int kColLabel   = 3;
static constexpr int kColAction  = 4;
static constexpr int kColValue   = 5;
static constexpr int kMaxRows    = 2000;

// Source → badge color map
static QColor source_color(const QString& source) {
    if (source == "keyboard")      { return QColor(0x44, 0xcc, 0x66); }
    if (source == "serial")        { return QColor(0xcc, 0xaa, 0x44); }
    if (source == "parallel_port") { return QColor(0xaa, 0x44, 0xcc); }
    if (source == "lsl")           { return QColor(0x44, 0xaa, 0xff); }
    return QColor(0x88, 0x88, 0xaa);
}

static QString source_abbrev(const QString& source) {
    if (source == "keyboard")      { return "KBD"; }
    if (source == "serial")        { return "SER"; }
    if (source == "parallel_port") { return "LPT"; }
    if (source == "lsl")           { return "LSL"; }
    return source.left(3).toUpper();
}

// Row data for filtering (stored alongside the table)
struct EventRow {
    QString wallTime;
    double  elapsedSec{0.0};
    QString source;
    QString label;
    TriggerAction action{TriggerAction::Log};
    double  value{0.0};
};

// ── Impl ───────────────────────────────────────────────────────────────────

struct TriggerEventPanelW::Impl {
    QTableWidget*          table      = nullptr;
    QComboBox*             sourceFilter = nullptr;
    QLineEdit*             labelSearch  = nullptr;
    QCheckBox*             autoScrollCk = nullptr;

    std::vector<EventRow>  allRows;         // full unfiltered history
    int64_t                sessionStartNs{0};  // reset when first event arrives

    static constexpr int k_max_rows = kMaxRows;
};

// ── Constructor ────────────────────────────────────────────────────────────

TriggerEventPanelW::TriggerEventPanelW(TriggerManager* manager, QWidget* parent)
    : QWidget(parent), d(std::make_unique<Impl>())
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    build_filter_bar();
    build_table();

    if (manager) {
        connect(manager, &TriggerManager::event_received,
                this,    &TriggerEventPanelW::on_event_received,
                Qt::QueuedConnection);
    }
}

TriggerEventPanelW::~TriggerEventPanelW() = default;

// ── Filter bar ─────────────────────────────────────────────────────────────

void TriggerEventPanelW::build_filter_bar() {
    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    static_cast<QVBoxLayout*>(layout())->addLayout(row);

    // Source filter
    d->sourceFilter = new QComboBox;
    d->sourceFilter->setFixedWidth(130);
    d->sourceFilter->addItem("All sources", "");
    d->sourceFilter->addItem("⌨  Keyboard", "keyboard");
    d->sourceFilter->addItem("⎋  Serial",   "serial");
    d->sourceFilter->addItem("▪  Parallel",  "parallel_port");
    d->sourceFilter->addItem("〜 LSL",       "lsl");
    row->addWidget(new QLabel("Source:"));
    row->addWidget(d->sourceFilter);

    // Label search
    d->labelSearch = new QLineEdit;
    d->labelSearch->setPlaceholderText("Search label…");
    d->labelSearch->setClearButtonEnabled(true);
    row->addWidget(new QLabel("Label:"));
    row->addWidget(d->labelSearch, 1);

    // Auto-scroll
    d->autoScrollCk = new QCheckBox("Auto-scroll");
    d->autoScrollCk->setChecked(true);
    row->addWidget(d->autoScrollCk);

    // Clear button
    auto* clearBtn = new QPushButton("Clear");
    clearBtn->setFixedWidth(60);
    connect(clearBtn, &QPushButton::clicked, this, &TriggerEventPanelW::clear);
    row->addWidget(clearBtn);

    // Export CSV
    auto* exportBtn = new QPushButton("Export CSV…");
    connect(exportBtn, &QPushButton::clicked, this, &TriggerEventPanelW::export_csv);
    row->addWidget(exportBtn);

    connect(d->sourceFilter, &QComboBox::currentIndexChanged,
            this, &TriggerEventPanelW::apply_filter);
    connect(d->labelSearch, &QLineEdit::textChanged,
            this, &TriggerEventPanelW::apply_filter);
}

// ── Table ──────────────────────────────────────────────────────────────────

void TriggerEventPanelW::build_table() {
    d->table = new QTableWidget(0, 6);
    d->table->setHorizontalHeaderLabels(
        {"Wall time", "Elapsed (s)", "Source", "Label", "Action", "Value"});

    d->table->setSelectionBehavior(QAbstractItemView::SelectRows);
    d->table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    d->table->setAlternatingRowColors(true);
    d->table->setShowGrid(false);
    d->table->verticalHeader()->hide();
    d->table->horizontalHeader()->setHighlightSections(false);
    d->table->horizontalHeader()->setSectionResizeMode(kColLabel, QHeaderView::Stretch);
    d->table->horizontalHeader()->setSectionResizeMode(kColSource, QHeaderView::Fixed);
    d->table->setColumnWidth(kColTime,    90);
    d->table->setColumnWidth(kColElapsed, 80);
    d->table->setColumnWidth(kColSource,  64);
    d->table->setColumnWidth(kColAction,  110);
    d->table->setColumnWidth(kColValue,   60);
    d->table->setStyleSheet(R"(
        QTableWidget {
            background: #0c0c1a;
            alternate-background-color: #10101f;
            color: #c8c8e0;
            border: 1px solid #1e1e3a;
            border-radius: 4px;
            font-size: 11px;
        }
        QHeaderView::section {
            background: #13132a;
            color: #7878a0;
            border: none;
            border-bottom: 1px solid #1e1e3a;
            padding: 4px 6px;
            font-weight: bold;
        }
    )");

    static_cast<QVBoxLayout*>(layout())->addWidget(d->table);
}

// ── Event handling ─────────────────────────────────────────────────────────

void TriggerEventPanelW::on_event_received(const TriggerEvent& event) {
    // Record raw event for filtering / export
    EventRow row;
    row.wallTime   = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    row.elapsedSec = static_cast<double>(event.timestampNs) / 1e9;
    row.source     = event.source;
    row.label      = event.label;
    row.action     = event.action;
    row.value      = event.value;

    if (d->allRows.size() >= static_cast<size_t>(d->k_max_rows)) {
        d->allRows.erase(d->allRows.begin());
    }
    d->allRows.push_back(row);

    // Apply current filter to decide whether to show this row
    const QString srcFilter   = d->sourceFilter->currentData().toString();
    const QString labelFilter = d->labelSearch->text().trimmed().toLower();

    const bool srcOk   = srcFilter.isEmpty()   || row.source == srcFilter;
    const bool labelOk = labelFilter.isEmpty() || row.label.toLower().contains(labelFilter);
    if (!srcOk || !labelOk) { return; }

    // Prune table if too large
    if (d->table->rowCount() >= kMaxRows) {
        d->table->removeRow(0);
    }

    const int newRow = d->table->rowCount();
    d->table->insertRow(newRow);

    // Time
    auto* timeItem = new QTableWidgetItem(row.wallTime);
    timeItem->setForeground(QColor(0x66, 0x88, 0x66));
    d->table->setItem(newRow, kColTime, timeItem);

    // Elapsed
    auto* elapsedItem = new QTableWidgetItem(QString::number(row.elapsedSec, 'f', 3));
    elapsedItem->setForeground(QColor(0x88, 0x88, 0xaa));
    d->table->setItem(newRow, kColElapsed, elapsedItem);

    // Source badge
    const QColor clr = source_color(row.source);
    auto* srcItem = new QTableWidgetItem(source_abbrev(row.source));
    srcItem->setBackground(clr.darker(200));
    srcItem->setForeground(clr);
    srcItem->setTextAlignment(Qt::AlignCenter);
    d->table->setItem(newRow, kColSource, srcItem);

    // Label
    auto* labelItem = new QTableWidgetItem(row.label);
    d->table->setItem(newRow, kColLabel, labelItem);

    // Action
    const QString actionText = trigger_action_label(row.action);
    auto* actionItem = new QTableWidgetItem(actionText);
    if (row.action == TriggerAction::StartRecording) {
        actionItem->setForeground(QColor(0x44, 0xcc, 0x66));
    } else if (row.action == TriggerAction::StopRecording) {
        actionItem->setForeground(QColor(0xcc, 0x55, 0x44));
    } else {
        actionItem->setForeground(QColor(0x55, 0x55, 0x88));
    }
    d->table->setItem(newRow, kColAction, actionItem);

    // Value
    auto* valItem = new QTableWidgetItem(QString::number(row.value, 'g', 4));
    valItem->setForeground(QColor(0x88, 0x88, 0xaa));
    valItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    d->table->setItem(newRow, kColValue, valItem);

    d->table->setRowHeight(newRow, 22);

    if (d->autoScrollCk && d->autoScrollCk->isChecked()) {
        d->table->scrollToBottom();
    }
}

// ── Apply filter ───────────────────────────────────────────────────────────

void TriggerEventPanelW::apply_filter() {
    d->table->setRowCount(0);
    const QString srcFilter   = d->sourceFilter->currentData().toString();
    const QString labelFilter = d->labelSearch->text().trimmed().toLower();

    for (const EventRow& row : d->allRows) {
        const bool srcOk   = srcFilter.isEmpty()   || row.source == srcFilter;
        const bool labelOk = labelFilter.isEmpty() || row.label.toLower().contains(labelFilter);
        if (!srcOk || !labelOk) { continue; }

        const int r = d->table->rowCount();
        d->table->insertRow(r);

        auto* timeItem    = new QTableWidgetItem(row.wallTime);
        auto* elapsedItem = new QTableWidgetItem(QString::number(row.elapsedSec, 'f', 3));
        auto* srcItem     = new QTableWidgetItem(source_abbrev(row.source));
        auto* labelItem   = new QTableWidgetItem(row.label);
        auto* actionItem  = new QTableWidgetItem(trigger_action_label(row.action));
        auto* valItem     = new QTableWidgetItem(QString::number(row.value, 'g', 4));

        const QColor clr = source_color(row.source);
        timeItem->setForeground(QColor(0x66, 0x88, 0x66));
        elapsedItem->setForeground(QColor(0x88, 0x88, 0xaa));
        srcItem->setBackground(clr.darker(200));
        srcItem->setForeground(clr);
        srcItem->setTextAlignment(Qt::AlignCenter);
        valItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

        d->table->setItem(r, kColTime,    timeItem);
        d->table->setItem(r, kColElapsed, elapsedItem);
        d->table->setItem(r, kColSource,  srcItem);
        d->table->setItem(r, kColLabel,   labelItem);
        d->table->setItem(r, kColAction,  actionItem);
        d->table->setItem(r, kColValue,   valItem);
        d->table->setRowHeight(r, 22);
    }

    if (d->autoScrollCk && d->autoScrollCk->isChecked() && d->table->rowCount() > 0) {
        d->table->scrollToBottom();
    }
}

// ── Clear ──────────────────────────────────────────────────────────────────

void TriggerEventPanelW::clear() {
    d->table->setRowCount(0);
    d->allRows.clear();
}

// ── Export CSV ─────────────────────────────────────────────────────────────

void TriggerEventPanelW::export_csv() {
    const QString path = QFileDialog::getSaveFileName(
        this, "Export trigger events", QString(),
        "CSV files (*.csv);;All files (*)");
    if (path.isEmpty()) { return; }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) { return; }

    QTextStream out(&file);
    out << "wall_time,elapsed_sec,source,label,action,value\n";
    for (const EventRow& row : d->allRows) {
        out << row.wallTime << ","
            << QString::number(row.elapsedSec, 'f', 6) << ","
            << row.source << ","
            << row.label << ","
            << trigger_action_label(row.action) << ","
            << QString::number(row.value, 'g', 6) << "\n";
    }
}

} // namespace mosaic
