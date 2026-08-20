#include "ui/trigger/parallel_port_card_w.hpp"

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/anim_utils.hpp"

namespace mosaic {

struct ParallelPortCardW::Impl {
    ParallelPortConfig& config;
    int index{0};

    QWidget* body          = nullptr;
    QToolButton* expandBtn = nullptr;
    QLabel* nameLabel      = nullptr;
    // Purely visual "am I showing my settings right now" state — orthogonal
    // to config.enabled ("is this trigger source active"). The two must
    // never be wired to toggle each other: an unchecked-but-expanded card
    // shows its grayed-out body, a checked-but-collapsed card hides it.
    bool expanded = true;

    explicit Impl(ParallelPortConfig& cfg, int idx) : config(cfg), index(idx) {}
};

ParallelPortCardW::ParallelPortCardW(ParallelPortConfig& config, int index, QWidget* parent)
    : QWidget(parent), d(std::make_unique<Impl>(config, index)) {
    // Same header/body collapsible-card shell as CameraCardW/KeyboardCardW/
    // MicrophoneCardW/SerialCardW — this was previously the one "card" that
    // didn't follow it (a single checkable QGroupBox with no expand/collapse
    // and no header at all).
    setObjectName("ParallelPortCardW");
    setStyleSheet(R"(
        #ParallelPortCardW {
            background: #13132a;
            border: 1px solid #252545;
            border-radius: 6px;
        }
    )");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Header ───────────────────────────────────────────────────────────
    auto* header = new QWidget(this);
    header->setObjectName("ppHeader");
    header->setStyleSheet(R"(
        QWidget#ppHeader {
            background: #1a1a38;
            border-bottom: 1px solid #252545;
            border-radius: 6px 6px 0 0;
        }
    )");
    header->setFixedHeight(40);

    auto* headerLay = new QHBoxLayout(header);
    headerLay->setContentsMargins(8, 0, 8, 0);
    headerLay->setSpacing(6);

    auto* expandBtn = new QToolButton;
    expandBtn->setText("▼");
    expandBtn->setStyleSheet(
        "QToolButton { background: transparent; border: none;"
        " color: #6666aa; font-size: 10px; }");
    expandBtn->setCursor(Qt::PointingHandCursor);
    d->expandBtn = expandBtn;
    headerLay->addWidget(expandBtn);

    d->nameLabel = new QLabel(QString("Parallel Port %1").arg(index + 1));
    d->nameLabel->setStyleSheet(
        "font-weight: bold; font-size: 12px;"
        " color: #c8c8e0; background: transparent;");
    headerLay->addWidget(d->nameLabel);
    headerLay->addStretch();

    // "Is this trigger source active" — moved here from the old
    // QGroupBox::setCheckable(true), which conflated this with the card's
    // expanded/collapsed visual state (a concept that didn't separately
    // exist before this restructure). Still disables the body's contents
    // when off, matching the old checkable-groupbox behavior.
    auto* enabledCk = new QCheckBox("Active");
    enabledCk->setChecked(config.enabled);
    connect(enabledCk, &QCheckBox::toggled, this, [this](bool on) {
        d->config.enabled = on;
        d->body->setEnabled(on);
        emit config_changed();
    });
    headerLay->addWidget(enabledCk);

    connect(expandBtn, &QToolButton::clicked, this, [this] {
        d->expanded = !d->expanded;
        anim::animate_collapse(d->body, d->expanded);
        d->expandBtn->setText(d->expanded ? "▼" : "▶");
    });

    // ── Body ─────────────────────────────────────────────────────────────
    d->body = new QWidget(this);
    d->body->setStyleSheet("background: transparent;");
    d->body->setEnabled(config.enabled);

    auto* form = new QVBoxLayout(d->body);
    form->setContentsMargins(10, 10, 10, 10);
    form->setSpacing(6);

    // ── Port address ──────────────────────────────────────────────────────
    auto* addrRow = new QHBoxLayout;
    auto* addrLbl = new QLabel("Port address:");
    addrLbl->setFixedWidth(110);
    addrLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    addrRow->addWidget(addrLbl);

    auto* addrEdit = new QLineEdit(config.portAddress);
    addrEdit->setPlaceholderText("e.g. 0x378");
    addrEdit->setFixedWidth(100);
    connect(addrEdit, &QLineEdit::textChanged, this, [this](const QString& val) {
        d->config.portAddress = val;
        emit config_changed();
    });
    addrRow->addWidget(addrEdit);
    addrRow->addWidget(new QLabel("(LPT1 = 0x378, LPT2 = 0x278)"));
    addrRow->addStretch();
    form->addLayout(addrRow);

    // ── Poll rate ─────────────────────────────────────────────────────────
    auto* pollRow = new QHBoxLayout;
    auto* pollLbl = new QLabel("Poll rate:");
    pollLbl->setFixedWidth(110);
    pollLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    pollRow->addWidget(pollLbl);

    auto* pollSpin = new QSpinBox;
    pollSpin->setRange(1, 100);
    pollSpin->setValue(config.pollRateMs);
    pollSpin->setSuffix("  ms");
    pollSpin->setFixedWidth(90);
    connect(pollSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int val) {
        d->config.pollRateMs = val;
        emit config_changed();
    });
    pollRow->addWidget(pollSpin);
    pollRow->addStretch();
    form->addLayout(pollRow);

    // ── Invert logic ──────────────────────────────────────────────────────
    auto* invertCk = new QCheckBox("Invert logic (active-low TTL)");
    invertCk->setChecked(config.invertLogic);
    connect(invertCk, &QCheckBox::toggled, this, [this](bool val) {
        d->config.invertLogic = val;
        emit config_changed();
    });
    form->addWidget(invertCk);

    // ── Send recording marker ────────────────────────────────────────────
    auto* markerCk =
        new QCheckBox("Send recording start/stop marker on this port (Control-register INIT pin)");
    markerCk->setChecked(config.sendRecordingMarker);
    markerCk->setToolTip(
        "Drives pin 16 (Control register, INIT) high when a recording starts "
        "and low when it stops — physically separate pins from the Data "
        "register above, which keeps reading incoming triggers with no "
        "conflict. Lets an external device (e.g. an EEG amplifier) log "
        "Mosaic's own recording start/stop in its own trigger channel.");
    connect(markerCk, &QCheckBox::toggled, this, [this](bool val) {
        d->config.sendRecordingMarker = val;
        emit config_changed();
    });
    form->addWidget(markerCk);

    // ── Platform note ─────────────────────────────────────────────────────
    auto* noteLbl =
        new QLabel("Requires InpOut32.dll in the application directory (Windows only).");
    noteLbl->setProperty("role", "muted");
    noteLbl->setWordWrap(true);
    form->addWidget(noteLbl);

    // ── Remove button ─────────────────────────────────────────────────────
    auto* removeBtn = new QPushButton("Remove");
    removeBtn->setFlat(true);
    removeBtn->setFixedWidth(70);
    connect(removeBtn, &QPushButton::clicked, this, [this] { emit remove_requested(d->index); });
    auto* removeRow = new QHBoxLayout;
    removeRow->addStretch();
    removeRow->addWidget(removeBtn);
    form->addLayout(removeRow);

    root->addWidget(header);
    root->addWidget(d->body);
}

ParallelPortCardW::~ParallelPortCardW() = default;

void ParallelPortCardW::set_index(int index) {
    d->index = index;
    d->nameLabel->setText(QString("Parallel Port %1").arg(index + 1));
}

} // namespace mosaic
