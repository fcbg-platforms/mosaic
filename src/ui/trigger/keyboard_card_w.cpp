#include "ui/trigger/keyboard_card_w.hpp"

#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace mosaic {

KeyboardCardW::KeyboardCardW(KeyTriggerConfig& config, int index, QWidget* parent)
    : QWidget(parent), m_config(config), m_index(index) {
    setObjectName("KeyboardCardW");
    setStyleSheet(R"(
        #KeyboardCardW {
            background: #121a14;
            border: 1px solid #1e3322;
            border-radius: 6px;
        }
    )");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    build_header();
    build_body();

    root->addWidget(findChild<QWidget*>("kbHeader"));
    root->addWidget(m_body);
}

KeyboardCardW::~KeyboardCardW() = default;

// ── Header ─────────────────────────────────────────────────────────────────

void KeyboardCardW::build_header() {
    auto* header = new QWidget(this);
    header->setObjectName("kbHeader");
    header->setStyleSheet(R"(
        QWidget#kbHeader {
            background: #172019;
            border-bottom: 1px solid #1e3322;
            border-radius: 6px 6px 0 0;
        }
    )");
    header->setFixedHeight(40);

    auto* lay = new QHBoxLayout(header);
    lay->setContentsMargins(8, 0, 8, 0);
    lay->setSpacing(6);

    // Expand arrow
    auto* expandBtn = new QToolButton;
    expandBtn->setText("▼");
    expandBtn->setStyleSheet(
        "QToolButton { background: transparent; border: none;"
        " color: #448844; font-size: 10px; }");
    expandBtn->setCursor(Qt::PointingHandCursor);
    connect(expandBtn, &QToolButton::clicked, this, &KeyboardCardW::toggle_expanded);
    m_expandBtn = expandBtn;
    lay->addWidget(expandBtn);

    // Name label (bold, green tint)
    m_nameLabel = new QLabel(QString("Trigger %1").arg(m_index + 1));
    m_nameLabel->setStyleSheet(
        "font-weight: bold; font-size: 12px;"
        " color: #88dd88; background: transparent;");
    lay->addWidget(m_nameLabel);

    // Summary (key binding, muted)
    m_summaryLabel = new QLabel;
    m_summaryLabel->setStyleSheet(
        "color: #446644; font-size: 11px;"
        " background: transparent;");
    lay->addWidget(m_summaryLabel);
    update_header_summary();

    lay->addStretch();

    // Live counter — styled like an LCD
    m_counterLabel = new QLabel("0");
    m_counterLabel->setFixedWidth(52);
    m_counterLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_counterLabel->setStyleSheet(
        "font-family: 'Courier New', monospace;"
        " font-size: 14px; font-weight: bold;"
        " color: #44ee44; background: #060e06;"
        " border: 1px solid #1a3a1a; border-radius: 3px;"
        " padding: 1px 5px;");
    lay->addWidget(m_counterLabel);

    // Delete button
    auto* delBtn = new QToolButton;
    delBtn->setText("✕");
    delBtn->setStyleSheet(R"(
        QToolButton { background: transparent; border: none;
                      color: #445544; font-size: 13px; padding: 2px 4px; }
        QToolButton:hover { color: #cc4444; }
    )");
    delBtn->setCursor(Qt::PointingHandCursor);
    connect(delBtn, &QToolButton::clicked, this, [this] { emit remove_requested(m_index); });
    lay->addWidget(delBtn);
}

// ── Body ───────────────────────────────────────────────────────────────────

void KeyboardCardW::build_body() {
    m_body = new QWidget(this);
    m_body->setStyleSheet("background: transparent;");

    auto* lay = new QVBoxLayout(m_body);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(0);

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setSpacing(8);

    // Name field
    auto* nameEdit = new QLineEdit(m_config.name);
    nameEdit->setPlaceholderText("e.g. Stimulus onset");
    form->addRow("Name:", nameEdit);

    // Key sequence editor
    auto* keyEdit = new QKeySequenceEdit(QKeySequence(m_config.keySeq, QKeySequence::PortableText));
    keyEdit->setMaximumSequenceLength(1); // single key only
    form->addRow("Key binding:", keyEdit);

    // Enabled checkbox
    auto* enabledCk = new QCheckBox("Active");
    enabledCk->setChecked(m_config.enabled);
    form->addRow(enabledCk);

    lay->addLayout(form);

    // ── Reset counter button ───────────────────────────────────────────
    auto* resetRow = new QHBoxLayout;
    resetRow->addStretch();
    auto* resetBtn = new QPushButton("Reset counter");
    resetBtn->setFixedHeight(24);
    resetBtn->setProperty("flat", true);
    connect(resetBtn, &QPushButton::clicked, this, [this] { on_count_changed(0); });
    resetRow->addWidget(resetBtn);
    lay->addSpacing(4);
    lay->addLayout(resetRow);

    // Wire up
    connect(nameEdit, &QLineEdit::textEdited, this, [this](const QString& v) {
        m_config.name = v;
        update_header_summary();
        emit config_changed();
    });
    connect(keyEdit, &QKeySequenceEdit::keySequenceChanged, this, [this](const QKeySequence& seq) {
        m_config.keySeq = seq.toString(QKeySequence::PortableText);
        update_header_summary();
        emit config_changed();
    });
    connect(enabledCk, &QCheckBox::toggled, this, [this](bool v) {
        m_config.enabled = v;
        emit config_changed();
    });
}

// ── Helpers ────────────────────────────────────────────────────────────────

void KeyboardCardW::update_header_summary() {
    const QString key = m_config.keySeq.isEmpty() ? "unbound" : m_config.keySeq;
    m_summaryLabel->setText(QString("— %1  [%2]").arg(m_config.name, key));
}

void KeyboardCardW::on_count_changed(int count) {
    if (m_counterLabel) m_counterLabel->setText(QString::number(count));
}

void KeyboardCardW::set_index(int index) {
    m_index = index;
    if (m_nameLabel) m_nameLabel->setText(QString("Trigger %1").arg(index + 1));
}

void KeyboardCardW::toggle_expanded() {
    auto* btn  = qobject_cast<QToolButton*>(m_expandBtn);
    auto* anim = new QPropertyAnimation(m_body, "maximumHeight", this);
    anim->setDuration(180);
    anim->setEasingCurve(QEasingCurve::InOutCubic);

    if (m_expanded) {
        anim->setStartValue(m_body->height());
        anim->setEndValue(0);
        connect(anim, &QAbstractAnimation::finished, m_body, &QWidget::hide);
        if (btn) btn->setText("▶");
    } else {
        m_body->show();
        m_body->setMaximumHeight(0);
        anim->setStartValue(0);
        anim->setEndValue(m_body->sizeHint().height());
        connect(anim, &QAbstractAnimation::finished,
                [this] { m_body->setMaximumHeight(QWIDGETSIZE_MAX); });
        if (btn) btn->setText("▼");
    }
    anim->start(QAbstractAnimation::DeleteWhenStopped);
    m_expanded = !m_expanded;
}

} // namespace mosaic
