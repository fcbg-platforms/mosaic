#include "trigger/trigger_manager.hpp"

#include "trigger/keyboard_trigger.hpp"
#include "trigger/parallel_port_trigger.hpp"
#ifdef MOSAIC_HAVE_SERIAL
#include "trigger/serial_trigger.hpp"
#endif
#include "trigger/trigger_recorder.hpp"
#include "utils/logger.hpp"

namespace mosaic {

struct TriggerManager::Impl {
    TriggerSettings& settings;

    std::vector<std::unique_ptr<KeyboardTrigger>> keyTriggers;
#ifdef MOSAIC_HAVE_SERIAL
    std::vector<std::unique_ptr<SerialTrigger>> serialTriggers;
#endif
    std::vector<std::unique_ptr<ParallelPortTrigger>> portTriggers;
    std::unique_ptr<TriggerRecorder> recorder;

    explicit Impl(TriggerSettings& s)
        : settings(s), recorder(std::make_unique<TriggerRecorder>()) {}
};

TriggerManager::TriggerManager(TriggerSettings& settings, QObject* parent)
    : QObject(parent), d(std::make_unique<Impl>(settings)) {
    reload();
}

TriggerManager::~TriggerManager() = default;

// ── Reload ─────────────────────────────────────────────────────────────────

void TriggerManager::reload() {
    // ── Tear down existing sources ─────────────────────────────────────────
    for (auto& kt : d->keyTriggers) {
        kt->set_active(false);
    }
#ifdef MOSAIC_HAVE_SERIAL
    for (auto& st : d->serialTriggers) {
        st->close();
    }
#endif
    for (auto& pp : d->portTriggers) {
        pp->stop();
    }
    d->keyTriggers.clear();
#ifdef MOSAIC_HAVE_SERIAL
    d->serialTriggers.clear();
#endif
    d->portTriggers.clear();

    if (!d->settings.receiveEnabled) {
        log_info("[TriggerManager] Trigger receive disabled.");
        return;
    }

    // ── Keyboard triggers ──────────────────────────────────────────────────
    d->settings.keyboardTriggers.reserve(32);
    for (auto& cfg : d->settings.keyboardTriggers) {
        // A trigger defaults to enabled=true, keySeq="" — it looks active
        // but KeyboardTrigger::eventFilter() silently returns on every key
        // press until a key is actually bound. Warn here the same way
        // serial/parallel-port triggers already warn on a failed open,
        // below — this one has no equivalent open()/start() failure to
        // hang the warning off, so it's checked explicitly instead.
        if (cfg.enabled && cfg.keySeq.isEmpty()) {
            log_warning(QString("[TriggerManager] Keyboard trigger '%1' is Active but has no "
                                "key bound — it will never fire. Bind a key in the Triggers tab.")
                            .arg(cfg.name));
        }
        auto kt = std::make_unique<KeyboardTrigger>(cfg, this);
        connect(kt.get(), &KeyboardTrigger::triggered, this, &TriggerManager::on_trigger_fired);
        kt->set_active(true);
        d->keyTriggers.push_back(std::move(kt));
    }

    // ── Serial triggers ────────────────────────────────────────────────────
#ifdef MOSAIC_HAVE_SERIAL
    for (auto& cfg : d->settings.serialTriggers) {
        if (!cfg.enabled || cfg.portName.isEmpty()) {
            continue;
        }
        auto st = std::make_unique<SerialTrigger>(cfg, this);
        connect(st.get(), &SerialTrigger::triggered, this, &TriggerManager::on_trigger_fired);
        connect(st.get(), &SerialTrigger::error_occurred, this, [](const QString& msg) {
            log_error(QString("[TriggerManager] Serial error: %1").arg(msg));
        });
        if (!st->open()) {
            log_warning(
                QString("[TriggerManager] Serial port '%1' failed to open.").arg(cfg.portName));
        }
        d->serialTriggers.push_back(std::move(st));
    }
#endif

    // ── Parallel port triggers ─────────────────────────────────────────────
    for (auto& cfg : d->settings.parallelPorts) {
        if (!cfg.enabled) {
            continue;
        }
        auto ppt = std::make_unique<ParallelPortTrigger>(cfg, this);
        connect(ppt.get(), &ParallelPortTrigger::triggered, this,
                &TriggerManager::on_trigger_fired);
        connect(ppt.get(), &ParallelPortTrigger::error_occurred, this, [](const QString& msg) {
            log_error(QString("[TriggerManager] Parallel port error: %1").arg(msg));
        });
        const bool started = ppt->start();
        if (!started) {
            log_warning(QString("[TriggerManager] Parallel port 0x%1 failed to start.")
                            .arg(cfg.portAddress));
        }
        d->portTriggers.push_back(std::move(ppt));
    }

    log_info(QString("[TriggerManager] Reloaded: %1 keyboard, %2 serial, %3 parallel port(s).")
                 .arg(d->keyTriggers.size())
#ifdef MOSAIC_HAVE_SERIAL
                 .arg(d->serialTriggers.size())
#else
                 .arg(0)
#endif
                 .arg(d->portTriggers.size()));
}

// ── Action lookup ──────────────────────────────────────────────────────────

static TriggerAction lookup_action(const TriggerSettings& settings, const TriggerEvent& ev) {
    if (ev.source == "keyboard") {
        for (const auto& cfg : settings.keyboardTriggers) {
            if (cfg.name == ev.label) {
                return cfg.action;
            }
        }
    } else if (ev.source == "serial") {
        for (const auto& cfg : settings.serialTriggers) {
            if (cfg.name == ev.label) {
                return cfg.action;
            }
        }
    } else if (ev.source == "parallel_port") {
        if (!settings.parallelPorts.empty()) {
            return settings.parallelPorts[0].action;
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

// ── Recording lifecycle ────────────────────────────────────────────────────

void TriggerManager::start_recording(const QString& csvPath) {
    if (!d->recorder->start(csvPath)) {
        log_error(QString("[TriggerManager] Cannot open trigger log: %1").arg(csvPath));
    } else {
        log_info(QString("[TriggerManager] Trigger recorder started → %1").arg(csvPath));
        for (auto& pp : d->portTriggers) {
            if (pp->config().sendRecordingMarker) {
                pp->set_recording_marker(true);
            }
        }
    }
}

void TriggerManager::stop_recording() {
    for (auto& pp : d->portTriggers) {
        if (pp->config().sendRecordingMarker) {
            pp->set_recording_marker(false);
        }
    }
    d->recorder->stop();
    log_info("[TriggerManager] Trigger recorder stopped.");
}

bool TriggerManager::is_recording() const { return d->recorder->is_recording(); }

// ── Accessors ──────────────────────────────────────────────────────────────

int TriggerManager::keyboard_trigger_count() const {
    return static_cast<int>(d->keyTriggers.size());
}

QObject* TriggerManager::keyboard_trigger_at(int index) const {
    if (index < 0 || index >= static_cast<int>(d->keyTriggers.size())) {
        return nullptr;
    }
    return d->keyTriggers[static_cast<size_t>(index)].get();
}

} // namespace mosaic
