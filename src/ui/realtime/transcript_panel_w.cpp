#include "ui/realtime/transcript_panel_w.hpp"

#include <QLabel>
#include <QScrollBar>
#include <QTextEdit>
#include <QTime>
#include <QVBoxLayout>

namespace mosaic {

struct TranscriptPanelW::Impl {
    QTextEdit* history = nullptr;
    QLabel* tentative  = nullptr;
};

TranscriptPanelW::TranscriptPanelW(QWidget* parent) : QWidget(parent), d(std::make_unique<Impl>()) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);

    d->history = new QTextEdit;
    d->history->setReadOnly(true);
    d->history->setStyleSheet(
        "QTextEdit { background:transparent; border:none; color:#d0d0e8; font-size:12px; }");
    lay->addWidget(d->history, 1);

    d->tentative = new QLabel;
    d->tentative->setWordWrap(true);
    d->tentative->setStyleSheet("QLabel { color:#8888b8; font-size:12px; font-style:italic; }");
    lay->addWidget(d->tentative);
}

TranscriptPanelW::~TranscriptPanelW() = default;

void TranscriptPanelW::push_final(const QString& text) {
    if (text.trimmed().isEmpty()) return;
    auto* bar              = d->history->verticalScrollBar();
    const bool wasAtBottom = bar->value() >= bar->maximum() - 4;
    d->history->append(QString("[%1] %2").arg(QTime::currentTime().toString("HH:mm:ss"), text));
    if (wasAtBottom) {
        bar->setValue(bar->maximum());
    }
}

void TranscriptPanelW::set_tentative(const QString& text) { d->tentative->setText(text); }

void TranscriptPanelW::clear() {
    d->history->clear();
    d->tentative->clear();
}

void TranscriptPanelW::set_unavailable(const QString& reason) {
    clear();
    d->history->setPlainText(reason);
}

} // namespace mosaic
