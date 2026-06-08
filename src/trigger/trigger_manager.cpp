#include "trigger/trigger_manager.hpp"
#include "trigger/keyboard_trigger.hpp"
#include "trigger/lsl_inlet.hpp"
#include "trigger/lsl_outlet.hpp"
#include "trigger/parallel_port_trigger.hpp"
#include "trigger/serial_trigger.hpp"
#include "trigger/trigger_recorder.hpp"
#include "utils/logger.hpp"

namespace mosaic {

struct TriggerManager::Impl {
    TriggerSettings& settings;

    std::vector<std::unique_ptr<KeyboardTrigger>>      keyTriggers;
    std::vector<std::unique_ptr<SerialTrigger>>        serialTriggers;
    std::vector<std::unique_ptr<LslInlet>>             lslInlets;
    std::vector<std::unique_ptr<ParallelPortTrigger>>  portTriggers;
    std::unique_ptr<LslOutlet>                         lslOutlet;
    std::unique_ptr<TriggerRecorder>                   recorder;

    explicit Impl(TriggerSettings& s)
        : settings(s)
        , lslOutlet(std::make_unique<LslOutlet>())
        , recorder(std::make_unique<TriggerRecorder>())
    {}
};

TriggerManager::TriggerManager(TriggerSettings& settings, QObject* parent)
    : QObject(parent), d(std::make_unique<Impl>(settings))
{
    reload();
}

TriggerManager::~TriggerManager() = default;

// ── Reload ─────────────────────────────────────────────────────────────────

void TriggerManager::reload() {
    // ── Tear down existing sources ─────────────────────────────────────────
    for (auto& kt : d->keyTriggers)    { kt->set_active(false); }
    for (auto& st : d->serialTriggers) { st->close(); }
    for (auto& pp : d->portTriggers)   { pp->stop(); }
    for (auto& li : d->lslInlets)      { li->disconnect(); }
    d->keyTriggers.clear();
    d->serialTriggers.clear();
    d->portTriggers.clear();
    d->lslInlets.clear();

    if (!d->settings.receiveEnabled) {
        log_info("[TriggerManager] Trigger receive disabled.");
        return;
    }

    // ── Keyboard triggers ──────────────────────────────────────────────────
    d->settings.keyboardTriggers.reserve(32);
    for (auto& cfg : d->settings.keyboardTriggers) {
        auto kt = std::make_unique<KeyboardTrigger>(cfg, this);
        connect(kt.get(), &KeyboardTrigger::triggered,
                this,     &TriggerManager::on_trigger_fired);
        kt->set_active(true);
        d->keyTriggers.push_back(std::move(kt));
    }

    // ── Serial triggers ────────────────────────────────────────────────────
    for (auto& cfg : d->settings.serialTriggers) {
        if (!cfg.enabled || cfg.portName.isEmpty()) { continue; }
        auto st = std::make_unique<SerialTrigger>(cfg, this);
        connect(st.get(), &SerialTrigger::triggered,
                this,     &TriggerManager::on_trigger_fired);
        connect(st.get(), &SerialTrigger::error_occurred, this,
                [](const QString& msg) {
                    log_error(QString("[TriggerManager] Serial error: %1").arg(msg));
                });
        if (!st->open()) {
            log_warning(QString("[TriggerManager] Serial port '%1' failed to open.")
                            .arg(cfg.portName));
        }
        d->serialTriggers.push_back(std::move(st));
    }

    // ── LSL inlets ─────────────────────────────────────────────────────────
    for (auto& cfg : d->settings.lslInlets) {
        if (!cfg.enabled) { continue; }
        auto inlet = std::make_unique<LslInlet>(cfg, this);
        connect(inlet.get(), &LslInlet::received,
                this,         &TriggerManager::on_trigger_fired, Qt::QueuedConnection);
        connect(inlet.get(), &LslInlet::connection_lost, this,
                [](const QString& name) {
                    log_warning(QString("[TriggerManager] LSL inlet '%1' disconnected.").arg(name));
                });
        // Non-blocking resolve — if the stream is not up yet the inlet is silent.
        const bool connected = inlet->connect(0.5);
        if (!connected) {
            log_info(QString("[TriggerManager] LSL inlet '%1' not yet available — will retry.")
                         .arg(cfg.streamName));
        }
        d->lslInlets.push_back(std::move(inlet));
    }

    // ── Parallel port triggers ─────────────────────────────────────────────
    for (auto& cfg : d->settings.parallelPorts) {
        if (!cfg.enabled) { continue; }
        auto ppt = std::make_unique<ParallelPortTrigger>(cfg, this);
        connect(ppt.get(), &ParallelPortTrigger::triggered,
                this,       &TriggerManager::on_trigger_fired);
        connect(ppt.get(), &ParallelPortTrigger::error_occurred, this,
                [](const QString& msg) {
                    log_error(QString("[TriggerManager] Parallel port error: %1").arg(msg));
                });
        const bool started = ppt->start();
        if (!started) {
            log_warning(QString("[TriggerManager] Parallel port 0x%1 failed to start.")
                            .arg(cfg.portAddress));
        }
        d->portTriggers.push_back(std::move(ppt));
    }

    // ── LSL outlet ─────────────────────────────────────────────────────────
    d->lslOutlet->close();
    if (d->settings.lslOutletEnabled) {
        [[maybe_unused]] const bool opened =
            d->lslOutlet->open(d->settings.lslOutletName,
                                d->settings.lslOutletRate);
    }

    log_info(QString("[TriggerManager] Reloaded: %1 keyboard, %2 serial, %3 LSL inlet(s), "
                     "%4 parallel port(s). LSL outlet: %5.")
                 .arg(d->keyTriggers.size())
                 .arg(d->serialTriggers.size())
                 .arg(d->lslInlets.size())
                 .arg(d->portTriggers.size())
                 .arg(d->lslOutlet->is_open() ? "on" : "off"));
}

// ── Action lookup ──────────────────────────────────────────────────────────

static TriggerAction lookup_action(const TriggerSettings& settings,
                                   const TriggerEvent& ev) {
    if (ev.source == "keyboard") {
        for (const auto& cfg : settings.keyboardTriggers) {
            if (cfg.name == ev.label) { return cfg.action; }
        }
    } else if (ev.source == "serial") {
        for (const auto& cfg : settings.serialTriggers) {
            if (cfg.name == ev.label) { return cfg.action; }
        }
    } else if (ev.source == "parallel_port") {
        if (!settings.parallelPorts.empty()) { return settings.parallelPorts[0].action; }
    } else if (ev.source == "lsl") {
        for (const auto& cfg : settings.lslInlets) {
            if (cfg.enabled) { return cfg.action; }
        }
    }
    return TriggerAction::Log;
}

// ── Event routing ──────────────────────────────────────────────────────────

void TriggerManager::on_trigger_fired(TriggerEvent event) {
    event.action = lookup_action(d->settings, event);

    if (d->recorder->is_recording()) {
        d->recorder->record_event(event);
    }
    emit event_received(event);

    if (event.action == TriggerAction::StartRecording ||
        event.action == TriggerAction::StopRecording) {
        emit action_requested(event.action, event);
    }
}

// ── LSL frame marker ───────────────────────────────────────────────────────

void TriggerManager::publish_frame_marker(int cameraIndex,
                                           int64_t frameId,
                                           int64_t elapsedNs) {
    d->lslOutlet->push_frame(cameraIndex, frameId, elapsedNs);
}

// ── Recording lifecycle ────────────────────────────────────────────────────

void TriggerManager::start_recording(const QString& csvPath) {
    if (!d->recorder->start(csvPath)) {
        log_error(QString("[TriggerManager] Cannot open trigger log: %1").arg(csvPath));
    } else {
        log_info(QString("[TriggerManager] Trigger recorder started → %1").arg(csvPath));
        // Stamp session start in the LSL event outlet.
        d->lslOutlet->push_event("SESSION_START");
    }
}

void TriggerManager::stop_recording() {
    d->lslOutlet->push_event("SESSION_STOP");
    d->recorder->stop();
    log_info("[TriggerManager] Trigger recorder stopped.");
}

bool TriggerManager::is_recording() const { return d->recorder->is_recording(); }

// ── Accessors ──────────────────────────────────────────────────────────────

int TriggerManager::keyboard_trigger_count() const {
    return static_cast<int>(d->keyTriggers.size());
}

QObject* TriggerManager::keyboard_trigger_at(int index) const {
    if (index < 0 || index >= static_cast<int>(d->keyTriggers.size())) { return nullptr; }
    return d->keyTriggers[static_cast<size_t>(index)].get();
}

} // namespace mosaic
