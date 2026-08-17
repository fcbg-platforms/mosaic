#include "ui/audio/microphone_card_w.hpp"

#include <QAudioDevice>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMediaDevices>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace mosaic {

MicrophoneCardW::MicrophoneCardW(MicrophoneParameters& params, int index, QWidget* parent)
    : QWidget(parent), m_params(params), m_index(index) {
    setObjectName("MicrophoneCardW");
    setStyleSheet(R"(
        #MicrophoneCardW {
            background: #13132a;
            border: 1px solid #252545;
            border-radius: 6px;
        }
    )");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    build_header();
    build_body();

    root->addWidget(findChild<QWidget*>("micHeader"));
    root->addWidget(m_body);
}

MicrophoneCardW::~MicrophoneCardW() = default;

// ── Header ─────────────────────────────────────────────────────────────────

void MicrophoneCardW::build_header() {
    auto* header = new QWidget(this);
    header->setObjectName("micHeader");
    header->setStyleSheet(R"(
        QWidget#micHeader {
            background: #1a1a38;
            border-bottom: 1px solid #252545;
            border-radius: 6px 6px 0 0;
        }
    )");

    auto* mainRow = new QVBoxLayout(header);
    mainRow->setContentsMargins(8, 4, 8, 2);
    mainRow->setSpacing(2);

    // Top row: expand / icon / name / dot / delete
    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(6);

    auto* expandBtn = new QToolButton;
    expandBtn->setText("▼");
    expandBtn->setStyleSheet(
        "QToolButton { background: transparent; border: none;"
        " color: #6666aa; font-size: 10px; }");
    expandBtn->setCursor(Qt::PointingHandCursor);
    connect(expandBtn, &QToolButton::clicked, this, &MicrophoneCardW::toggle_expanded);
    m_expandBtn = expandBtn;
    topRow->addWidget(expandBtn);

    auto* icon = new QLabel("🎙");
    icon->setStyleSheet("font-size: 13px; background: transparent;");
    topRow->addWidget(icon);

    m_nameLabel = new QLabel(QString("Microphone %1").arg(m_index + 1));
    m_nameLabel->setStyleSheet(
        "font-weight: bold; font-size: 12px;"
        " color: #c8c8e0; background: transparent;");
    topRow->addWidget(m_nameLabel);
    topRow->addStretch();

    // Status dot (green when recording, grey otherwise)
    auto* dot = new QLabel;
    dot->setFixedSize(10, 10);
    dot->setStyleSheet("background: #333355; border-radius: 5px;");
    m_statusDot = dot;
    topRow->addWidget(dot);

    auto* delBtn = new QToolButton;
    delBtn->setText("✕");
    delBtn->setStyleSheet(R"(
        QToolButton { background: transparent; border: none;
                      color: #555575; font-size: 13px; padding: 2px 4px; }
        QToolButton:hover { color: #cc4444; }
    )");
    delBtn->setCursor(Qt::PointingHandCursor);
    connect(delBtn, &QToolButton::clicked, this, [this] { emit remove_requested(m_index); });
    topRow->addWidget(delBtn);

    mainRow->addLayout(topRow);

    // Level meter bar (thin, shown below the title row)
    m_levelBar = new QProgressBar;
    m_levelBar->setRange(0, 100);
    m_levelBar->setValue(0);
    m_levelBar->setTextVisible(false);
    m_levelBar->setFixedHeight(4);
    m_levelBar->setStyleSheet(R"(
        QProgressBar {
            background: #0a0a1a;
            border: none;
            border-radius: 2px;
        }
        QProgressBar::chunk {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                stop:0    #1aaa44,
                stop:0.65 #66cc22,
                stop:0.85 #ccaa22,
                stop:1.0  #cc4422);
            border-radius: 2px;
        }
    )");
    mainRow->addWidget(m_levelBar);
}

// ── Body ───────────────────────────────────────────────────────────────────

void MicrophoneCardW::build_body() {
    m_body = new QWidget(this);
    m_body->setStyleSheet("background: transparent;");

    auto* lay = new QVBoxLayout(m_body);
    lay->setContentsMargins(8, 10, 8, 10);
    lay->setSpacing(0);

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setSpacing(8);

    // ── Device selector ───────────────────────────────────────────────────
    auto* deviceCombo = new QComboBox;
    deviceCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    const QList<QAudioDevice> devices = QMediaDevices::audioInputs();
    if (devices.isEmpty()) {
        deviceCombo->addItem("No input devices found", QString{});
        deviceCombo->setEnabled(false);
    } else {
        for (const QAudioDevice& dev : devices)
            deviceCombo->addItem(dev.description(), QString::fromLatin1(dev.id()));

        // Pre-select stored device (or default if not found)
        bool found = false;
        if (!m_params.deviceId.isEmpty()) {
            for (int i = 0; i < deviceCombo->count(); ++i) {
                if (deviceCombo->itemData(i).toString() == m_params.deviceId) {
                    deviceCombo->setCurrentIndex(i);
                    found = true;
                    break;
                }
            }
        }
        if (!found && !devices.isEmpty()) {
            // Default: select the system default input
            const QString defId = QString::fromLatin1(QMediaDevices::defaultAudioInput().id());
            for (int i = 0; i < deviceCombo->count(); ++i) {
                if (deviceCombo->itemData(i).toString() == defId) {
                    deviceCombo->setCurrentIndex(i);
                    break;
                }
            }
            // Persist the selection immediately
            m_params.deviceId     = deviceCombo->currentData().toString();
            m_params.friendlyName = deviceCombo->currentText();
        }
    }
    form->addRow("Device:", deviceCombo);

    // ── Sample rate ───────────────────────────────────────────────────────
    auto* rateCombo = new QComboBox;
    for (int r : {8000, 16000, 22050, 44100, 48000, 88200, 96000})
        rateCombo->addItem(QString::number(r) + " Hz", r);
    rateCombo->setCurrentText(QString::number(m_params.sampleRate) + " Hz");
    form->addRow("Sample rate:", rateCombo);

    // ── Channels ──────────────────────────────────────────────────────────
    auto* chSpin = new QSpinBox;
    chSpin->setRange(1, 8);
    chSpin->setValue(m_params.channels);
    chSpin->setSuffix("  ch");
    chSpin->setFixedWidth(90);
    form->addRow("Channels:", chSpin);

    // ── Buffer size ───────────────────────────────────────────────────────
    auto* bufCombo = new QComboBox;
    for (int b : {64, 128, 256, 512, 1024, 2048})
        bufCombo->addItem(QString::number(b) + " frames", b);
    bufCombo->setCurrentText(QString::number(m_params.bufferSize) + " frames");
    form->addRow("Buffer size:", bufCombo);

    lay->addLayout(form);

    // ── Wire up ───────────────────────────────────────────────────────────
    connect(deviceCombo, &QComboBox::currentIndexChanged, this, [this, deviceCombo](int idx) {
        m_params.deviceId     = deviceCombo->itemData(idx).toString();
        m_params.friendlyName = deviceCombo->itemText(idx);
        emit params_changed();
    });
    connect(rateCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, rateCombo](int idx) {
                m_params.sampleRate = rateCombo->itemData(idx).toInt();
                emit params_changed();
            });
    connect(chSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
        m_params.channels = v;
        emit params_changed();
    });
    connect(bufCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, bufCombo](int idx) {
                m_params.bufferSize = bufCombo->itemData(idx).toInt();
                emit params_changed();
            });
}

// ── Public API ─────────────────────────────────────────────────────────────

void MicrophoneCardW::set_level(float rms) {
    if (!m_levelBar) return;
    // Uses the shared "Scale" control (see AudioSettingsW::
    // build_waveform_section()) instead of an independent, separately-tuned
    // magic constant, so this VU meter and the waveform widget always agree
    // on how "hot" a given RMS level looks.
    const int val = static_cast<int>(std::clamp(rms * m_scale * 100.0f, 0.0f, 100.0f));
    m_levelBar->setValue(val);
}

void MicrophoneCardW::set_scale(float scale) { m_scale = scale; }

void MicrophoneCardW::set_index(int index) {
    m_index = index;
    if (m_nameLabel) m_nameLabel->setText(QString("Microphone %1").arg(index + 1));
}

void MicrophoneCardW::set_connected(bool connected) {
    if (m_statusDot)
        m_statusDot->setStyleSheet(connected ? "background: #44cc88; border-radius: 5px;"
                                             : "background: #333355; border-radius: 5px;");
}

// ── Collapse / expand ──────────────────────────────────────────────────────

void MicrophoneCardW::toggle_expanded() {
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
