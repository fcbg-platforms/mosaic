#include "ui/video/camera_card_w.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace mosaic {

// ── Helpers ────────────────────────────────────────────────────────────────

static QDoubleSpinBox* make_dspin(double min, double max, double val,
                                  double step = 1.0, int decimals = 1) {
    auto* w = new QDoubleSpinBox;
    w->setRange(min, max);
    w->setSingleStep(step);
    w->setDecimals(decimals);
    w->setValue(val);
    w->setFixedWidth(100);
    return w;
}

static QSpinBox* make_spin(int min, int max, int val, int step = 1) {
    auto* w = new QSpinBox;
    w->setRange(min, max);
    w->setSingleStep(step);
    w->setValue(val);
    w->setFixedWidth(100);
    return w;
}

static QComboBox* make_combo(const QStringList& items, const QString& current) {
    auto* w = new QComboBox;
    w->addItems(items);
    // A non-editable QComboBox::setCurrentText() silently does nothing if
    // `current` isn't one of `items` — the box would show whatever item 0
    // happens to be while the underlying CameraParameters field still holds
    // the real (different) value, and the next unrelated edit on this card
    // would overwrite it with the wrong displayed value. Append it instead
    // so a stored value from a saved config or a different camera model
    // (e.g. an older HW-trigger source no longer offered by default) always
    // stays visible and round-trips correctly.
    if (!current.isEmpty() && !items.contains(current)) {
        w->addItem(current);
    }
    w->setCurrentText(current);
    return w;
}

// Thin horizontal separator line inside a form layout
static void add_separator(QFormLayout* form) {
    auto* line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    form->addRow(line);
}

// Small bold section label inside a form layout
static void add_section(QFormLayout* form, const QString& text) {
    auto* lbl = new QLabel(text);
    lbl->setProperty("role", "section");
    form->addRow(lbl);
}

// Wraps a spinbox + unit label in a row widget
static QWidget* with_unit(QWidget* spin, const QString& unit) {
    auto* row = new QWidget;
    auto* lay = new QHBoxLayout(row);
    lay->setContentsMargins(0,0,0,0);
    lay->setSpacing(4);
    lay->addWidget(spin);
    auto* lbl = new QLabel(unit);
    lbl->setProperty("role", "unit");
    lay->addWidget(lbl);
    lay->addStretch();
    return row;
}

// ── Constructor ────────────────────────────────────────────────────────────

CameraCardW::CameraCardW(CameraParameters& params, int index, QWidget* parent)
    : QWidget(parent), m_params(params), m_index(index)
{
    setObjectName("CameraCardW");
    setStyleSheet(R"(
        #CameraCardW {
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

    root->addWidget(findChild<QWidget*>("cardHeader"));
    root->addWidget(m_body);
}

CameraCardW::~CameraCardW() = default;

// ── Header ─────────────────────────────────────────────────────────────────

void CameraCardW::build_header() {
    auto* header = new QWidget(this);
    header->setObjectName("cardHeader");
    header->setStyleSheet(R"(
        QWidget#cardHeader {
            background: #1a1a38;
            border-bottom: 1px solid #252545;
            border-radius: 6px 6px 0 0;
        }
    )");
    header->setFixedHeight(40);

    auto* lay = new QHBoxLayout(header);
    lay->setContentsMargins(8, 0, 8, 0);
    lay->setSpacing(6);

    // Expand / collapse arrow
    auto* expandBtn = new QToolButton;
    expandBtn->setText("▼");
    expandBtn->setStyleSheet("QToolButton { background: transparent; border: none;"
                             " color: #6666aa; font-size: 10px; }");
    expandBtn->setCursor(Qt::PointingHandCursor);
    connect(expandBtn, &QToolButton::clicked, this, &CameraCardW::toggle_expanded);
    m_expandBtn = expandBtn;
    lay->addWidget(expandBtn);

    // Always show 1-based position. set_index() updates this after sibling deletions.
    m_nameLabel = new QLabel(QString("Camera %1").arg(m_index + 1));
    m_nameLabel->setStyleSheet("font-weight: bold; font-size: 12px;"
                               " color: #c8c8e0; background: transparent;");
    lay->addWidget(m_nameLabel);

    // Serial number — editable. This is the only thing that ties a card's
    // configuration to a specific physical camera: VideoGrabber::open()
    // attaches by serial number, and falls back to "first device found" if
    // left empty, which is unsafe with more than one camera on the system
    // (every empty-serial grabber could attach to the same physical unit).
    auto* snEdit = new QLineEdit(m_params.serialNumber);
    snEdit->setObjectName("serialEdit");
    snEdit->setPlaceholderText("Serial number (required)");
    snEdit->setToolTip("Camera serial number — identifies which physical camera "
                       "this configuration applies to. Read it off the camera's "
                       "label or via Pylon IP Configurator's device list.");
    snEdit->setFixedWidth(120);
    snEdit->setStyleSheet(R"(
        QLineEdit#serialEdit {
            background: transparent; border: none; border-bottom: 1px solid transparent;
            color: #7070a0; font-size: 11px; padding: 1px 2px;
        }
        QLineEdit#serialEdit:hover  { border-bottom: 1px solid #333366; }
        QLineEdit#serialEdit:focus  { color: #c8c8e0; border-bottom: 1px solid #4444aa; }
    )");
    m_serialEdit = snEdit;
    lay->addWidget(snEdit);
    connect(snEdit, &QLineEdit::editingFinished, this, [this]{
        m_params.serialNumber = m_serialEdit->text().trimmed();
        emit params_changed();
    });

    lay->addStretch();

    // Connection status dot (gray = not connected, green = connected)
    auto* dot = new QLabel;
    dot->setFixedSize(10, 10);
    dot->setStyleSheet("background: #333355; border-radius: 5px;");
    m_statusDot = dot;
    lay->addWidget(dot);

    // Delete button
    auto* delBtn = new QToolButton;
    delBtn->setText("✕");
    delBtn->setStyleSheet(R"(
        QToolButton { background: transparent; border: none; color: #555575;
                      font-size: 13px; padding: 2px 4px; }
        QToolButton:hover { color: #cc4444; }
    )");
    delBtn->setCursor(Qt::PointingHandCursor);
    connect(delBtn, &QToolButton::clicked, this, [this]{
        emit remove_requested(m_index);
    });
    lay->addWidget(delBtn);
}

// ── Body ───────────────────────────────────────────────────────────────────

void CameraCardW::build_body() {
    m_body = new QWidget(this);
    m_body->setStyleSheet("background: transparent;");

    auto* lay = new QVBoxLayout(m_body);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(0);

    auto* tabs = new QTabWidget;
    tabs->setDocumentMode(false);

    auto* imgTab  = new QWidget; build_image_tab(imgTab);
    auto* expTab  = new QWidget; build_exposure_tab(expTab);
    auto* gainTab = new QWidget; build_gain_tab(gainTab);
    auto* advTab  = new QWidget; build_advanced_tab(advTab);
    auto* hwTrigTab = new QWidget; build_hw_trigger_tab(hwTrigTab);

    tabs->addTab(imgTab,    "Image");
    tabs->addTab(expTab,    "Exposure");
    tabs->addTab(gainTab,   "Gain");
    tabs->addTab(advTab,    "Advanced");
    tabs->addTab(hwTrigTab, "HW Trigger");

    lay->addWidget(tabs);
}

// ── Image tab ──────────────────────────────────────────────────────────────

void CameraCardW::build_image_tab(QWidget* tab) {
    auto* form = new QFormLayout(tab);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setSpacing(6);
    form->setContentsMargins(8, 8, 8, 8);

    add_section(form, "Resolution");

    auto* wSpin = make_spin(1, 65535, m_params.width,  2);
    auto* hSpin = make_spin(1, 65535, m_params.height, 2);
    form->addRow("Width:",  with_unit(wSpin, "px"));
    form->addRow("Height:", with_unit(hSpin, "px"));

    auto* oxSpin = make_spin(0, 65534, m_params.offsetX, 2);
    auto* oySpin = make_spin(0, 65534, m_params.offsetY, 2);
    form->addRow("Offset X:", with_unit(oxSpin, "px"));
    form->addRow("Offset Y:", with_unit(oySpin, "px"));

    add_separator(form);
    add_section(form, "Orientation");

    auto* rxCk = new QCheckBox("Reverse X");
    auto* ryCk = new QCheckBox("Reverse Y");
    rxCk->setChecked(m_params.reverseX);
    ryCk->setChecked(m_params.reverseY);
    form->addRow(rxCk);
    form->addRow(ryCk);

    add_separator(form);
    add_section(form, "Acquisition");

    auto* fmtCombo = make_combo({"BGR8","RGB8","Mono8","Mono12","BayerRG8","BayerBG8"},
                                m_params.pixelFormat);
    form->addRow("Pixel format:", fmtCombo);

    auto* fpsCk   = new QCheckBox("Specify frame rate");
    auto* fpsSpin = make_dspin(1.0, 200.0, m_params.fps, 1.0, 1);
    fpsCk->setChecked(m_params.specifyFps);
    fpsSpin->setEnabled(m_params.specifyFps);
    form->addRow(fpsCk);
    form->addRow("Frame rate:", with_unit(fpsSpin, "fps"));

    // ── Wire up ────────────────────────────────────────────────────────────
    connect(wSpin,   qOverload<int>(&QSpinBox::valueChanged),    this, [this](int v){ m_params.width  = v; emit params_changed(); });
    connect(hSpin,   qOverload<int>(&QSpinBox::valueChanged),    this, [this](int v){ m_params.height = v; emit params_changed(); });
    connect(oxSpin,  qOverload<int>(&QSpinBox::valueChanged),    this, [this](int v){ m_params.offsetX = v; emit params_changed(); });
    connect(oySpin,  qOverload<int>(&QSpinBox::valueChanged),    this, [this](int v){ m_params.offsetY = v; emit params_changed(); });
    connect(rxCk,    &QCheckBox::toggled, this, [this](bool v){ m_params.reverseX = v; emit params_changed(); });
    connect(ryCk,    &QCheckBox::toggled, this, [this](bool v){ m_params.reverseY = v; emit params_changed(); });
    connect(fmtCombo,&QComboBox::currentTextChanged, this, [this](const QString& v){ m_params.pixelFormat = v; emit params_changed(); });
    connect(fpsCk, &QCheckBox::toggled, this, [this, fpsSpin](bool v){
        m_params.specifyFps = v;
        fpsSpin->setEnabled(v);
        emit params_changed();
    });
    connect(fpsSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v){ m_params.fps = v; emit params_changed(); });
}

// ── Exposure tab ───────────────────────────────────────────────────────────

void CameraCardW::build_exposure_tab(QWidget* tab) {
    auto* form = new QFormLayout(tab);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setSpacing(6);
    form->setContentsMargins(8, 8, 8, 8);

    static const QStringList kAutoModes{"Off", "Once", "Continuous"};

    auto* autoCombo = make_combo(kAutoModes, m_params.exposureAuto);
    form->addRow("Auto mode:", autoCombo);

    add_separator(form);
    add_section(form, "Manual");

    auto* timeSpin = make_dspin(10.0, 1'000'000.0, m_params.exposureTimeUs, 100.0, 1);
    form->addRow("Exposure time:", with_unit(timeSpin, "µs"));

    add_separator(form);
    add_section(form, "Auto range");

    auto* lowerSpin = make_dspin(10.0, 1'000'000.0, m_params.exposureAutoLowerUs, 100.0, 1);
    auto* upperSpin = make_dspin(10.0, 1'000'000.0, m_params.exposureAutoUpperUs, 100.0, 1);
    form->addRow("Lower limit:", with_unit(lowerSpin, "µs"));
    form->addRow("Upper limit:", with_unit(upperSpin, "µs"));

    // Enable manual/auto-range controls based on mode
    auto update_enabled = [timeSpin, lowerSpin, upperSpin](const QString& mode) {
        timeSpin->setEnabled(mode == "Off");
        lowerSpin->setEnabled(mode != "Off");
        upperSpin->setEnabled(mode != "Off");
    };
    update_enabled(m_params.exposureAuto);
    connect(autoCombo, &QComboBox::currentTextChanged, this, [this, update_enabled](const QString& v){
        m_params.exposureAuto = v; update_enabled(v); emit params_changed();
    });
    connect(timeSpin,  qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v){ m_params.exposureTimeUs      = v; emit params_changed(); });
    connect(lowerSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v){ m_params.exposureAutoLowerUs = v; emit params_changed(); });
    connect(upperSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v){ m_params.exposureAutoUpperUs = v; emit params_changed(); });
}

// ── Gain tab ───────────────────────────────────────────────────────────────

void CameraCardW::build_gain_tab(QWidget* tab) {
    auto* form = new QFormLayout(tab);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setSpacing(6);
    form->setContentsMargins(8, 8, 8, 8);

    static const QStringList kAutoModes{"Off", "Once", "Continuous"};

    auto* autoCombo = make_combo(kAutoModes, m_params.gainAuto);
    form->addRow("Auto mode:", autoCombo);

    add_separator(form);
    add_section(form, "Manual");

    auto* gainSpin = make_dspin(0.0, 48.0, m_params.gainDb, 0.1, 2);
    form->addRow("Gain:", with_unit(gainSpin, "dB"));

    add_separator(form);
    add_section(form, "Auto range");

    auto* lowerSpin = make_dspin(0.0, 48.0, m_params.gainAutoLowerDb, 0.1, 2);
    auto* upperSpin = make_dspin(0.0, 48.0, m_params.gainAutoUpperDb, 0.1, 2);
    form->addRow("Lower limit:", with_unit(lowerSpin, "dB"));
    form->addRow("Upper limit:", with_unit(upperSpin, "dB"));

    auto update_enabled = [gainSpin, lowerSpin, upperSpin](const QString& mode) {
        gainSpin->setEnabled(mode == "Off");
        lowerSpin->setEnabled(mode != "Off");
        upperSpin->setEnabled(mode != "Off");
    };
    update_enabled(m_params.gainAuto);
    connect(autoCombo, &QComboBox::currentTextChanged, this, [this, update_enabled](const QString& v){
        m_params.gainAuto = v; update_enabled(v); emit params_changed();
    });
    connect(gainSpin,  qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v){ m_params.gainDb          = v; emit params_changed(); });
    connect(lowerSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v){ m_params.gainAutoLowerDb = v; emit params_changed(); });
    connect(upperSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v){ m_params.gainAutoUpperDb = v; emit params_changed(); });
}

// ── Advanced tab ───────────────────────────────────────────────────────────

void CameraCardW::build_advanced_tab(QWidget* tab) {
    auto* form = new QFormLayout(tab);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setSpacing(6);
    form->setContentsMargins(8, 8, 8, 8);

    add_section(form, "Tone & colour");

    auto* gammaSpin  = make_dspin(0.1,  4.0,  m_params.gamma,      0.05, 2);
    auto* blSpin     = make_dspin(0.0,  63.0, m_params.blackLevel,  0.5,  1);
    auto* satSpin    = make_dspin(0.0,  2.0,  m_params.saturation,  0.05, 2);
    auto* contSpin   = make_dspin(0.0,  2.0,  m_params.contrast,    0.05, 2);
    auto* brightSpin = make_dspin(0.0,  1.0,  m_params.brightness,  0.01, 2);
    auto* atbSpin    = make_dspin(0.0,  1.0,  m_params.autoTargetBrightness, 0.01, 2);
    auto* dshSpin    = make_spin (0,    4,    m_params.digitalShift, 1);

    // Saturation/Contrast/Brightness have no GenICam node on every camera
    // generation Mosaic has been run against (e.g. the ace-classic GigE
    // cameras used in room 11 have no on-camera ISP for these) — they're
    // saved with the session but only take effect on hardware that exposes
    // the matching node.
    const QString noHwNote = "May not be supported by every camera model — "
                              "saved regardless, applied only if the camera exposes it.";
    satSpin->setToolTip(noHwNote);
    contSpin->setToolTip(noHwNote);
    brightSpin->setToolTip(noHwNote);

    form->addRow("Gamma:",                gammaSpin);
    form->addRow("Black level:",          blSpin);
    form->addRow("Saturation:",           satSpin);
    form->addRow("Contrast:",             contSpin);
    form->addRow("Brightness:",           brightSpin);
    form->addRow("Auto target bright.:",  atbSpin);
    form->addRow("Digital shift (bits):", with_unit(dshSpin, "bit"));

    add_separator(form);
    add_section(form, "White balance");

    static const QStringList kAutoModes{"Off", "Once", "Continuous"};
    auto* bwCombo = make_combo(kAutoModes, m_params.balanceWhiteAuto);
    form->addRow("Auto mode:", bwCombo);

    add_separator(form);
    add_section(form, "Test pattern (simulation)");

    auto* patCombo = make_combo({"Off","ColorBars","Horizontal","Vertical"},
                                m_params.testPattern);
    form->addRow("Pattern:", patCombo);

    connect(gammaSpin,  qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double val){ m_params.gamma            = val; emit params_changed(); });
    connect(blSpin,     qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double val){ m_params.blackLevel       = val; emit params_changed(); });
    connect(satSpin,    qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double val){ m_params.saturation       = val; emit params_changed(); });
    connect(contSpin,   qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double val){ m_params.contrast         = val; emit params_changed(); });
    connect(brightSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double val){ m_params.brightness       = val; emit params_changed(); });
    connect(atbSpin,    qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double val){ m_params.autoTargetBrightness = val; emit params_changed(); });
    connect(dshSpin,    qOverload<int>   (&QSpinBox::valueChanged),       this, [this](int    val){ m_params.digitalShift     = val; emit params_changed(); });
    connect(bwCombo,    &QComboBox::currentTextChanged, this, [this](const QString& val){ m_params.balanceWhiteAuto = val; emit params_changed(); });
    connect(patCombo,   &QComboBox::currentTextChanged, this, [this](const QString& val){ m_params.testPattern      = val; emit params_changed(); });
}

// ── HW Trigger tab ─────────────────────────────────────────────────────────

void CameraCardW::build_hw_trigger_tab(QWidget* tab) {
    auto* form = new QFormLayout(tab);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setSpacing(8);
    form->setContentsMargins(8, 8, 8, 8);

    add_section(form, "Hardware trigger input");

    auto* enabledCk = new QCheckBox("Enable hardware trigger");
    enabledCk->setChecked(m_params.hwTriggerEnabled);
    form->addRow(enabledCk);

    // Options match the real TriggerSource enum on the Basler ace-classic
    // GigE cameras this was verified against — Line1 is the opto-isolated
    // input, Action1 is a GigE Vision scheduled action command, Software
    // lets the SDK drive the trigger for testing without external wiring.
    auto* srcCombo = make_combo({"Line1", "Software", "Action1"},
                                m_params.hwTriggerSource);
    srcCombo->setEnabled(m_params.hwTriggerEnabled);
    form->addRow("Trigger source:", srcCombo);

    auto* delaySpin = make_dspin(0.0, 1'000'000.0, m_params.hwTriggerDelayUs, 10.0, 1);
    delaySpin->setEnabled(m_params.hwTriggerEnabled);
    form->addRow("Trigger delay:", with_unit(delaySpin, "µs"));

    add_separator(form);
    auto* hint = new QLabel(
        "Hardware trigger mode allows an external TTL pulse (e.g. from a "
        "stimulus controller, parallel port, or another camera) to trigger "
        "each frame acquisition for precise synchronisation.\n\n"
        "Line1 is the standard Basler opto-isolated input.  "
        "Software mode lets the SDK drive the trigger for testing.");
    hint->setWordWrap(true);
    hint->setProperty("role", "muted");
    form->addRow(hint);

    auto update_states = [srcCombo, delaySpin](bool enabled) {
        srcCombo->setEnabled(enabled);
        delaySpin->setEnabled(enabled);
    };

    connect(enabledCk, &QCheckBox::toggled, this, [this, update_states](bool val){
        m_params.hwTriggerEnabled = val;
        update_states(val);
        emit params_changed();
    });
    connect(srcCombo, &QComboBox::currentTextChanged, this, [this](const QString& val){
        m_params.hwTriggerSource = val; emit params_changed();
    });
    connect(delaySpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double val){
        m_params.hwTriggerDelayUs = val; emit params_changed();
    });
}

// ── Collapse / expand ──────────────────────────────────────────────────────

void CameraCardW::toggle_expanded() {
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
        connect(anim, &QAbstractAnimation::finished, [this]{
            m_body->setMaximumHeight(QWIDGETSIZE_MAX);
        });
        if (btn) btn->setText("▼");
    }
    anim->start(QAbstractAnimation::DeleteWhenStopped);
    m_expanded = !m_expanded;
}

// ── Public API ─────────────────────────────────────────────────────────────

void CameraCardW::set_index(int index) {
    m_index = index;
    if (m_nameLabel)
        m_nameLabel->setText(QString("Camera %1").arg(index + 1));
}

void CameraCardW::set_connected(bool connected) {
    if (m_statusDot)
        m_statusDot->setStyleSheet(connected
            ? "background: #44cc88; border-radius: 5px;"
            : "background: #333355; border-radius: 5px;");
}

void CameraCardW::refresh() {
    if (m_serialEdit) {
        m_serialEdit->setText(m_params.serialNumber);
    }
    // TODO: walk remaining child controls and reload from m_params.
    // For now the caller can simply recreate the card.
}

} // namespace mosaic
