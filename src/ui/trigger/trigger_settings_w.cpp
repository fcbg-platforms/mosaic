#include "ui/trigger/trigger_settings_w.hpp"
#include "ui/trigger/keyboard_card_w.hpp"
#ifdef MOSAIC_HAVE_SERIAL
#include "ui/trigger/serial_card_w.hpp"
#include "trigger/serial_trigger.hpp"
#endif
#include "trigger/keyboard_trigger.hpp"
#include "utils/timestamp.hpp"
#include <QCheckBox>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTextEdit>
#include <QVector>

namespace mosaic {

struct TriggerSettingsW::Impl {
    // Master / LSL outlet controls
    QCheckBox* masterCk      = nullptr;
    QCheckBox* lslOutletCk   = nullptr;
    QLineEdit* lslNameEdit   = nullptr;

    // Keyboard cards
    QVBoxLayout*             keyboardLayout = nullptr;
    QVector<KeyboardCardW*>  keyCards;

    // Serial trigger cards
#ifdef MOSAIC_HAVE_SERIAL
    QVBoxLayout*             serialLayout = nullptr;
    QVector<SerialCardW*>    serialCards;
#endif

    // LSL inlet placeholder
    QWidget* lslInletPlaceholder = nullptr;

    // Live event log
    QTextEdit* eventLog = nullptr;
    int        logLineCount{0};
    static constexpr int k_maxLogLines = 200;
};

// ── Constructor ────────────────────────────────────────────────────────────

TriggerSettingsW::TriggerSettingsW(TriggerSettings& settings,
                                    TriggerManager*  manager,
                                    QWidget*         parent)
    : QWidget(parent), m_settings(settings), m_manager(manager),
      d(std::make_unique<Impl>())
{
    auto* outerLay = new QVBoxLayout(this);
    outerLay->setContentsMargins(0, 0, 0, 0);
    outerLay->setSpacing(0);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content    = new QWidget;
    auto* contentLay = new QVBoxLayout(content);
    contentLay->setContentsMargins(10, 10, 10, 10);
    contentLay->setSpacing(10);

    build_master_section(contentLay);
    build_lsl_outlet_section(contentLay);
    build_keyboard_section(contentLay);
    build_serial_section(contentLay);
    build_lsl_inlet_section(contentLay);
    build_event_log(contentLay);
    contentLay->addStretch();

    scroll->setWidget(content);
    outerLay->addWidget(scroll);

    // Pre-reserve so references stay valid
    m_settings.keyboardTriggers.reserve(32);
    m_settings.serialTriggers.reserve(16);

    // Create cards for existing keyboard and serial triggers
    for (int i = 0; i < static_cast<int>(m_settings.keyboardTriggers.size()); ++i) {
        make_keyboard_card(i);
    }
#ifdef MOSAIC_HAVE_SERIAL
    for (int i = 0; i < static_cast<int>(m_settings.serialTriggers.size()); ++i) {
        make_serial_card(i);
    }
#endif

    // Connect to live event feed
    if (m_manager) {
        connect(m_manager, &TriggerManager::event_received,
                this, &TriggerSettingsW::on_event_received,
                Qt::QueuedConnection);
    }
}

TriggerSettingsW::~TriggerSettingsW() = default;

// ── Master toggle ──────────────────────────────────────────────────────────

void TriggerSettingsW::build_master_section(QVBoxLayout* parent) {
    auto* box = new QGroupBox("Trigger Reception");
    auto* lay = new QVBoxLayout(box);
    lay->setSpacing(6);

    d->masterCk = new QCheckBox("Receive triggers");
    d->masterCk->setChecked(m_settings.receiveEnabled);
    d->masterCk->setStyleSheet(
        "QCheckBox { font-size: 13px; font-weight: bold; spacing: 8px; }"
        "QCheckBox::indicator { width: 18px; height: 18px; }");
    lay->addWidget(d->masterCk);

    auto* hint = new QLabel(
        "When disabled, all trigger sources are paused but their "
        "configuration is preserved.");
    hint->setWordWrap(true);
    hint->setProperty("role", "muted");
    lay->addWidget(hint);

    connect(d->masterCk, &QCheckBox::toggled, this, [this](bool v){
        m_settings.receiveEnabled = v;
        reload_manager();
        emit settings_changed();
    });

    parent->addWidget(box);
}

// ── LSL outlet ─────────────────────────────────────────────────────────────

void TriggerSettingsW::build_lsl_outlet_section(QVBoxLayout* parent) {
    auto* box = new QGroupBox("LSL Outlet  (frame timestamp publisher)");
    auto* lay = new QVBoxLayout(box);
    lay->setSpacing(8);

    d->lslOutletCk = new QCheckBox("Publish frame timestamps via LSL");
    d->lslOutletCk->setChecked(m_settings.lslOutletEnabled);
    lay->addWidget(d->lslOutletCk);

    auto* nameRow = new QHBoxLayout;
    auto* nameLbl = new QLabel("Stream name:");
    nameLbl->setFixedWidth(95);
    nameLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    d->lslNameEdit = new QLineEdit(m_settings.lslOutletName);
    d->lslNameEdit->setEnabled(m_settings.lslOutletEnabled);
    nameRow->addWidget(nameLbl);
    nameRow->addWidget(d->lslNameEdit, 1);
    lay->addLayout(nameRow);

#ifndef MOSAIC_HAVE_LSL
    auto* noLsl = new QLabel(
        "⚠  liblsl not found. Configure with -DMOSAIC_ENABLE_LSL=ON "
        "and set LSL_ROOT to enable. Keyboard triggers still work.");
    noLsl->setWordWrap(true);
    noLsl->setStyleSheet("color: #aa8833; font-size: 11px;");
    lay->addWidget(noLsl);
    d->lslOutletCk->setEnabled(false);
    d->lslNameEdit->setEnabled(false);
#endif

    connect(d->lslOutletCk, &QCheckBox::toggled, this, [this](bool v){
        m_settings.lslOutletEnabled = v;
        d->lslNameEdit->setEnabled(v);
        emit settings_changed();
    });
    connect(d->lslNameEdit, &QLineEdit::textEdited, this, [this](const QString& v){
        m_settings.lslOutletName = v;
        emit settings_changed();
    });

    parent->addWidget(box);
}

// ── Keyboard triggers ──────────────────────────────────────────────────────

void TriggerSettingsW::build_keyboard_section(QVBoxLayout* parent) {
    auto* headerRow = new QHBoxLayout;

    auto* lbl = new QLabel("Keyboard Triggers");
    lbl->setProperty("role", "section");
    headerRow->addWidget(lbl);
    headerRow->addStretch();

    auto* addBtn = new QPushButton("+ Add trigger");
    addBtn->setFixedHeight(26);
    connect(addBtn, &QPushButton::clicked, this, [this]{
        add_keyboard_trigger({});
    });
    headerRow->addWidget(addBtn);
    parent->addLayout(headerRow);

    auto* cardsWidget = new QWidget;
    d->keyboardLayout = new QVBoxLayout(cardsWidget);
    d->keyboardLayout->setContentsMargins(0, 0, 0, 0);
    d->keyboardLayout->setSpacing(6);
    parent->addWidget(cardsWidget);
}

void TriggerSettingsW::make_keyboard_card(int index) {
    auto* card = new KeyboardCardW(m_settings.keyboardTriggers[index], index, this);

    connect(card, &KeyboardCardW::config_changed, this, [this]{
        reload_manager();
        emit settings_changed();
    });
    connect(card, &KeyboardCardW::remove_requested,
            this, &TriggerSettingsW::remove_keyboard_trigger);

    // Connect the matching KeyboardTrigger's counter to the card's display.
    if (m_manager) {
        if (auto* kt = qobject_cast<KeyboardTrigger*>(
                m_manager->keyboard_trigger_at(index))) {
            connect(kt, &KeyboardTrigger::count_changed,
                    card, &KeyboardCardW::on_count_changed,
                    Qt::QueuedConnection);
        }
    }

    d->keyboardLayout->addWidget(card);
    d->keyCards.append(card);
}

void TriggerSettingsW::add_keyboard_trigger(KeyTriggerConfig cfg) {
    m_settings.keyboardTriggers.push_back(std::move(cfg));
    reload_manager();
    make_keyboard_card(static_cast<int>(m_settings.keyboardTriggers.size()) - 1);
    emit settings_changed();
}

void TriggerSettingsW::remove_keyboard_trigger(int index) {
    if (index < 0 || index >= d->keyCards.size()) return;

    auto* card = d->keyCards[index];
    d->keyboardLayout->removeWidget(card);
    card->deleteLater();
    d->keyCards.remove(index);
    m_settings.keyboardTriggers.erase(
        m_settings.keyboardTriggers.begin() + index);

    reload_manager();

    for (int i = index; i < d->keyCards.size(); ++i)
        d->keyCards[i]->set_index(i);

    emit settings_changed();
}

// ── Serial triggers ────────────────────────────────────────────────────────

void TriggerSettingsW::build_serial_section(QVBoxLayout* parent) {
#ifdef MOSAIC_HAVE_SERIAL
    auto* headerRow = new QHBoxLayout;
    auto* lbl = new QLabel("Serial Port Triggers");
    lbl->setProperty("role", "section");
    headerRow->addWidget(lbl);
    headerRow->addStretch();

    auto* addBtn = new QPushButton("+ Add serial trigger");
    addBtn->setFixedHeight(26);
    connect(addBtn, &QPushButton::clicked, this, [this]{
        add_serial_trigger({});
    });
    headerRow->addWidget(addBtn);
    parent->addLayout(headerRow);

    auto* cardsWidget = new QWidget;
    d->serialLayout = new QVBoxLayout(cardsWidget);
    d->serialLayout->setContentsMargins(0, 0, 0, 0);
    d->serialLayout->setSpacing(6);
    parent->addWidget(cardsWidget);

    auto* hint = new QLabel(
        "Receive trigger bytes from RS-232 / USB-serial devices  "
        "(function generators, custom hardware, EEG amplifiers, etc.).");
    hint->setWordWrap(true);
    hint->setStyleSheet("color: #555577; font-size: 11px;"
                        " background: #0d0d1e; border: 1px solid #1e1e3a;"
                        " border-radius: 4px; padding: 8px;");
    parent->addWidget(hint);
#else
    auto* note = new QLabel(
        "Serial port triggers require Qt6::SerialPort. Install it via "
        "the Qt Maintenance Tool to enable RS-232 / USB-serial trigger support.");
    note->setWordWrap(true);
    note->setStyleSheet("color: #555577; font-size: 11px;"
                        " background: #0d0d1e; border: 1px solid #1e1e3a;"
                        " border-radius: 4px; padding: 8px;");
    parent->addWidget(note);
#endif
}

#ifdef MOSAIC_HAVE_SERIAL
void TriggerSettingsW::make_serial_card(int index) {
    auto* card = new SerialCardW(m_settings.serialTriggers[index], index, this);
    connect(card, &SerialCardW::config_changed, this, [this]{
        reload_manager();
        emit settings_changed();
    });
    connect(card, &SerialCardW::remove_requested,
            this, &TriggerSettingsW::remove_serial_trigger);
    d->serialLayout->addWidget(card);
    d->serialCards.append(card);
}

void TriggerSettingsW::add_serial_trigger(SerialTriggerConfig cfg) {
    m_settings.serialTriggers.push_back(std::move(cfg));
    reload_manager();
    make_serial_card(static_cast<int>(m_settings.serialTriggers.size()) - 1);
    emit settings_changed();
}

void TriggerSettingsW::remove_serial_trigger(int index) {
    if (index < 0 || index >= d->serialCards.size()) { return; }

    auto* card = d->serialCards[index];
    d->serialLayout->removeWidget(card);
    card->deleteLater();
    d->serialCards.remove(index);
    m_settings.serialTriggers.erase(m_settings.serialTriggers.begin() + index);

    reload_manager();

    for (int i = index; i < d->serialCards.size(); ++i) {
        d->serialCards[i]->set_index(i);
    }
    emit settings_changed();
}
#else
void TriggerSettingsW::make_serial_card(int) {}
void TriggerSettingsW::add_serial_trigger(SerialTriggerConfig) {}
void TriggerSettingsW::remove_serial_trigger(int) {}
#endif

// ── LSL inlets ─────────────────────────────────────────────────────────────

void TriggerSettingsW::build_lsl_inlet_section(QVBoxLayout* parent) {
    auto* headerRow = new QHBoxLayout;
    auto* lbl = new QLabel("LSL Inlets  (receive markers from other devices)");
    lbl->setProperty("role", "section");
    headerRow->addWidget(lbl);
    headerRow->addStretch();
    parent->addLayout(headerRow);

#ifdef MOSAIC_HAVE_LSL
    auto* addBtn = new QPushButton("+ Add LSL inlet");
    addBtn->setFixedHeight(26);
    // TODO: implement LSL inlet cards in the next iteration
    headerRow->addWidget(addBtn);
#else
    auto* note = new QLabel(
        "LSL inlets require liblsl. Once installed and configured "
        "with -DMOSAIC_ENABLE_LSL=ON, EEG amplifiers, eye trackers, "
        "and any LSL-compatible device will appear here.");
    note->setWordWrap(true);
    note->setStyleSheet("color: #555577; font-size: 11px;"
                        " background: #0d0d1e; border: 1px solid #1e1e3a;"
                        " border-radius: 4px; padding: 8px;");
    parent->addWidget(note);
#endif
}

// ── Live event log ─────────────────────────────────────────────────────────

void TriggerSettingsW::build_event_log(QVBoxLayout* parent) {
    auto* box = new QGroupBox("Live Event Log");
    auto* lay = new QVBoxLayout(box);

    d->eventLog = new QTextEdit;
    d->eventLog->setReadOnly(true);
    d->eventLog->setMinimumHeight(120);
    d->eventLog->setMaximumHeight(200);
    d->eventLog->setStyleSheet(
        "QTextEdit { background: #060a06; color: #44cc44;"
        " font-family: 'Courier New', monospace; font-size: 11px;"
        " border: 1px solid #1a2a1a; border-radius: 4px; padding: 4px; }");

    auto* clearBtn = new QPushButton("Clear");
    clearBtn->setProperty("flat", true);
    clearBtn->setFixedHeight(22);
    connect(clearBtn, &QPushButton::clicked, this, [this]{
        d->eventLog->clear();
        d->logLineCount = 0;
    });

    lay->addWidget(d->eventLog);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    btnRow->addWidget(clearBtn);
    lay->addLayout(btnRow);

    parent->addWidget(box);
}

void TriggerSettingsW::on_event_received(const TriggerEvent& event) {
    if (!d->eventLog) return;

    // Trim log if it grows too long
    if (d->logLineCount >= Impl::k_maxLogLines) {
        d->eventLog->clear();
        d->logLineCount = 0;
    }

    const QString timeStr = wall_clock_string("hh:mm:ss.zzz");

    // Color by source
    QString color = "#44cc44";     // keyboard → green
    if (event.source == "lsl")    color = "#44aaff";
    if (event.source == "serial") color = "#ccaa44";

    const QString line = QString(
        "<span style='color:#336633'>%1</span>"
        "  <span style='color:%2;font-weight:bold'>[%3]</span>"
        "  <span style='color:#aaddaa'>%4</span>")
        .arg(timeStr, color,
             event.source.toUpper().leftJustified(3, ' '),
             event.label.toHtmlEscaped());

    d->eventLog->append(line);
    ++d->logLineCount;
}

// ── Manager sync ───────────────────────────────────────────────────────────

void TriggerSettingsW::reload_manager() {
    if (m_manager) m_manager->reload();
}

} // namespace mosaic
