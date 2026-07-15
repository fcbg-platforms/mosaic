#include "ui/trigger/parallel_port_card_w.hpp"
#include <QCheckBox>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace mosaic {

struct ParallelPortCardW::Impl {
    ParallelPortConfig& config;
    int                 index{0};
    QGroupBox*          box     = nullptr;

    explicit Impl(ParallelPortConfig& cfg, int idx)
        : config(cfg), index(idx) {}
};

ParallelPortCardW::ParallelPortCardW(ParallelPortConfig& config,
                                      int                index,
                                      QWidget*           parent)
    : QWidget(parent), d(std::make_unique<Impl>(config, index))
{
    auto* outerLay = new QVBoxLayout(this);
    outerLay->setContentsMargins(0, 0, 0, 0);

    d->box = new QGroupBox(QString("Parallel Port %1").arg(index + 1));
    d->box->setCheckable(true);
    d->box->setChecked(config.enabled);
    connect(d->box, &QGroupBox::toggled, this, [this](bool on) {
        d->config.enabled = on;
        emit config_changed();
    });

    auto* form = new QVBoxLayout(d->box);
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

    // ── Platform note ─────────────────────────────────────────────────────
    auto* noteLbl = new QLabel(
        "Requires InpOut32.dll in the application directory (Windows only).");
    noteLbl->setProperty("role", "muted");
    noteLbl->setWordWrap(true);
    form->addWidget(noteLbl);

    // ── Remove button ─────────────────────────────────────────────────────
    auto* removeBtn = new QPushButton("Remove");
    removeBtn->setFlat(true);
    removeBtn->setFixedWidth(70);
    connect(removeBtn, &QPushButton::clicked, this, [this] {
        emit remove_requested(d->index);
    });
    auto* removeRow = new QHBoxLayout;
    removeRow->addStretch();
    removeRow->addWidget(removeBtn);
    form->addLayout(removeRow);

    outerLay->addWidget(d->box);
}

ParallelPortCardW::~ParallelPortCardW() = default;

void ParallelPortCardW::set_index(int index) {
    d->index = index;
    d->box->setTitle(QString("Parallel Port %1").arg(index + 1));
}

} // namespace mosaic
