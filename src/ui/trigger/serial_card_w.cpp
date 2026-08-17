#include "ui/trigger/serial_card_w.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPropertyAnimation>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include "trigger/serial_trigger.hpp"
#include "trigger/trigger_types.hpp"

namespace mosaic {

SerialCardW::SerialCardW(SerialTriggerConfig& config, int index, QWidget* parent)
    : QWidget(parent), m_config(config), m_index(index) {
    setObjectName("SerialCardW");
    setStyleSheet(R"(
        #SerialCardW {
            background: #131320;
            border: 1px solid #252545;
            border-radius: 6px;
        }
    )");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    build_header();
    build_body();

    root->addWidget(findChild<QWidget*>("serialHeader"));
    root->addWidget(m_body);
}

SerialCardW::~SerialCardW() = default;

// ── Header ─────────────────────────────────────────────────────────────────

void SerialCardW::build_header() {
    auto* header = new QWidget(this);
    header->setObjectName("serialHeader");
    header->setStyleSheet(R"(
        QWidget#serialHeader {
            background: #1a1a38;
            border-bottom: 1px solid #252545;
            border-radius: 6px 6px 0 0;
        }
    )");
    header->setFixedHeight(52);
    header->setCursor(Qt::PointingHandCursor);

    auto* outerLay = new QVBoxLayout(header);
    outerLay->setContentsMargins(8, 4, 8, 4);
    outerLay->setSpacing(2);

    // ── Top row ───────────────────────────────────────────────────────────
    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(6);

    auto* expandBtn = new QToolButton;
    expandBtn->setText("▼");
    expandBtn->setStyleSheet(
        "QToolButton { background: transparent; border: none;"
        " color: #6666aa; font-size: 10px; }");
    expandBtn->setCursor(Qt::PointingHandCursor);
    connect(expandBtn, &QToolButton::clicked, this, &SerialCardW::toggle_expanded);
    m_expandBtn = expandBtn;
    topRow->addWidget(expandBtn);

    auto* icon = new QLabel("⎋"); // serial/connector icon
    icon->setStyleSheet("font-size: 14px; background: transparent; color: #ccaa44;");
    topRow->addWidget(icon);

    m_nameLabel = new QLabel(m_config.name.isEmpty() ? "Serial Trigger" : m_config.name);
    m_nameLabel->setStyleSheet(
        "font-weight: bold; font-size: 12px;"
        " color: #c8c8e0; background: transparent;");
    topRow->addWidget(m_nameLabel);
    topRow->addStretch();

    m_counterLabel = new QLabel("0 fires");
    m_counterLabel->setStyleSheet("color: #6666aa; font-size: 10px; background: transparent;");
    topRow->addWidget(m_counterLabel);

    auto* delBtn = new QToolButton;
    delBtn->setText("✕");
    delBtn->setStyleSheet(R"(
        QToolButton { background: transparent; border: none; color: #555575;
                      font-size: 13px; padding: 2px 4px; }
        QToolButton:hover { color: #cc4444; }
    )");
    delBtn->setCursor(Qt::PointingHandCursor);
    connect(delBtn, &QToolButton::clicked, this, [this] { emit remove_requested(m_index); });
    topRow->addWidget(delBtn);

    outerLay->addLayout(topRow);

    // ── Summary row ───────────────────────────────────────────────────────
    m_summaryLabel = new QLabel;
    m_summaryLabel->setStyleSheet(
        "color: #5555aa; font-size: 10px; background: transparent;"
        " margin-left: 24px;");
    update_header_summary();
    outerLay->addWidget(m_summaryLabel);
}

// ── Body ───────────────────────────────────────────────────────────────────

void SerialCardW::build_body() {
    m_body = new QWidget(this);
    m_body->setStyleSheet("background: transparent;");

    auto* lay = new QVBoxLayout(m_body);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(0);

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setSpacing(8);

    // ── Name ──────────────────────────────────────────────────────────────
    auto* nameEdit = new QLineEdit(m_config.name);
    nameEdit->setPlaceholderText("Trigger label");
    form->addRow("Name:", nameEdit);

    // ── Enabled toggle ────────────────────────────────────────────────────
    auto* enabledCk = new QCheckBox("Active");
    enabledCk->setChecked(m_config.enabled);
    form->addRow("", enabledCk);

    // ── Port name ─────────────────────────────────────────────────────────
    auto* portCombo = new QComboBox;
    portCombo->setEditable(true);
    const QStringList ports = SerialTrigger::available_ports();
    portCombo->addItems(ports);
    if (!m_config.portName.isEmpty()) {
        const int idx = portCombo->findText(m_config.portName);
        if (idx >= 0) {
            portCombo->setCurrentIndex(idx);
        } else {
            portCombo->setCurrentText(m_config.portName);
        }
    }
    form->addRow("Port:", portCombo);

    // ── Baud rate ─────────────────────────────────────────────────────────
    auto* baudCombo = new QComboBox;
    for (int baud : {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600}) {
        baudCombo->addItem(QString::number(baud), baud);
    }
    baudCombo->setCurrentText(QString::number(m_config.baudRate));
    form->addRow("Baud rate:", baudCombo);

    // ── Data bits / parity / stop bits in one row ─────────────────────────
    auto* dataCombo = new QComboBox;
    for (int db : {5, 6, 7, 8}) {
        dataCombo->addItem(QString::number(db), db);
    }
    dataCombo->setCurrentText(QString::number(m_config.dataBits));
    dataCombo->setFixedWidth(60);

    auto* parityCombo = new QComboBox;
    for (const char* p : {"None", "Even", "Odd"}) {
        parityCombo->addItem(p);
    }
    parityCombo->setCurrentText(m_config.parity);
    parityCombo->setFixedWidth(70);

    auto* stopCombo = new QComboBox;
    stopCombo->addItem("1", 1.0);
    stopCombo->addItem("1.5", 1.5);
    stopCombo->addItem("2", 2.0);
    stopCombo->setCurrentText(QString::number(m_config.stopBits));
    stopCombo->setFixedWidth(50);

    auto* frameRow = new QHBoxLayout;
    frameRow->setSpacing(4);
    frameRow->addWidget(dataCombo);
    frameRow->addWidget(new QLabel("N"));
    frameRow->addWidget(parityCombo);
    frameRow->addWidget(stopCombo);
    frameRow->addStretch();
    form->addRow("Frame:", frameRow);

    // ── Match mode ────────────────────────────────────────────────────────
    auto* matchCombo = new QComboBox;
    matchCombo->addItem("Any byte", "AnyByte");
    matchCombo->addItem("Non-zero", "NonZero");
    matchCombo->addItem("Exact byte", "Exact");
    {
        const int idx = matchCombo->findData(m_config.matchMode);
        if (idx >= 0) {
            matchCombo->setCurrentIndex(idx);
        }
    }
    form->addRow("Match:", matchCombo);

    auto* matchValEdit = new QLineEdit(m_config.matchValue);
    matchValEdit->setPlaceholderText("0xFF");
    matchValEdit->setEnabled(m_config.matchMode == "Exact");
    form->addRow("Match value:", matchValEdit);

    // ── Action ────────────────────────────────────────────────────────────
    auto* actionCombo = new QComboBox;
    for (const TriggerAction act :
         {TriggerAction::Log, TriggerAction::StartRecording, TriggerAction::StopRecording}) {
        actionCombo->addItem(trigger_action_label(act), static_cast<int>(act));
    }
    actionCombo->setCurrentText(trigger_action_label(m_config.action));
    form->addRow("Action:", actionCombo);

    lay->addLayout(form);

    // ── Wire up ───────────────────────────────────────────────────────────
    connect(nameEdit, &QLineEdit::textEdited, this, [this](const QString& v) {
        m_config.name = v;
        if (m_nameLabel) {
            m_nameLabel->setText(v.isEmpty() ? "Serial Trigger" : v);
        }
        update_header_summary();
        emit config_changed();
    });
    connect(enabledCk, &QCheckBox::toggled, this, [this](bool v) {
        m_config.enabled = v;
        emit config_changed();
    });
    connect(portCombo, &QComboBox::currentTextChanged, this, [this](const QString& v) {
        m_config.portName = v;
        update_header_summary();
        emit config_changed();
    });
    connect(baudCombo, &QComboBox::currentIndexChanged, this, [this, baudCombo](int idx) {
        m_config.baudRate = baudCombo->itemData(idx).toInt();
        update_header_summary();
        emit config_changed();
    });
    connect(dataCombo, &QComboBox::currentIndexChanged, this, [this, dataCombo](int idx) {
        m_config.dataBits = dataCombo->itemData(idx).toInt();
        emit config_changed();
    });
    connect(parityCombo, &QComboBox::currentTextChanged, this, [this](const QString& v) {
        m_config.parity = v;
        emit config_changed();
    });
    connect(stopCombo, &QComboBox::currentIndexChanged, this, [this, stopCombo](int idx) {
        m_config.stopBits = stopCombo->itemData(idx).toDouble();
        emit config_changed();
    });
    connect(matchCombo, &QComboBox::currentIndexChanged, this,
            [this, matchCombo, matchValEdit](int idx) {
                m_config.matchMode = matchCombo->itemData(idx).toString();
                matchValEdit->setEnabled(m_config.matchMode == "Exact");
                emit config_changed();
            });
    connect(matchValEdit, &QLineEdit::textEdited, this, [this](const QString& v) {
        m_config.matchValue = v;
        emit config_changed();
    });
    connect(actionCombo, &QComboBox::currentIndexChanged, this, [this, actionCombo](int idx) {
        m_config.action = static_cast<TriggerAction>(actionCombo->itemData(idx).toInt());
        emit config_changed();
    });
}

// ── Collapse / expand ──────────────────────────────────────────────────────

void SerialCardW::toggle_expanded() {
    auto* btn  = qobject_cast<QToolButton*>(m_expandBtn);
    auto* anim = new QPropertyAnimation(m_body, "maximumHeight", this);
    anim->setDuration(180);
    anim->setEasingCurve(QEasingCurve::InOutCubic);

    if (m_expanded) {
        anim->setStartValue(m_body->height());
        anim->setEndValue(0);
        connect(anim, &QAbstractAnimation::finished, m_body, &QWidget::hide);
        if (btn) {
            btn->setText("▶");
        }
    } else {
        m_body->show();
        m_body->setMaximumHeight(0);
        anim->setStartValue(0);
        anim->setEndValue(m_body->sizeHint().height());
        connect(anim, &QAbstractAnimation::finished,
                [this] { m_body->setMaximumHeight(QWIDGETSIZE_MAX); });
        if (btn) {
            btn->setText("▼");
        }
    }
    anim->start(QAbstractAnimation::DeleteWhenStopped);
    m_expanded = !m_expanded;
}

// ── Public API ─────────────────────────────────────────────────────────────

void SerialCardW::on_count_changed(int count) {
    if (!m_counterLabel) {
        return;
    }
    m_counterLabel->setText(QString("%1 fires").arg(count));
}

void SerialCardW::set_index(int index) { m_index = index; }

void SerialCardW::update_header_summary() {
    if (!m_summaryLabel) {
        return;
    }
    if (m_config.portName.isEmpty()) {
        m_summaryLabel->setText("No port selected");
    } else {
        m_summaryLabel->setText(QString("%1  @  %2 baud  —  %3")
                                    .arg(m_config.portName)
                                    .arg(m_config.baudRate)
                                    .arg(trigger_action_label(m_config.action)));
    }
}

} // namespace mosaic
