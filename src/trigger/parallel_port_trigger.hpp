#pragma once
#include <QObject>
#include <memory>

#include "core/settings.hpp"
#include "trigger/trigger_types.hpp"

namespace mosaic {

// Monitors a parallel port data register (LPT) for bit-level changes and
// fires TriggerEvents on rising and falling edges.
//
// Implementation:
//   Windows + MOSAIC_HAVE_PARALLEL_PORT: dynamically loads InpOut32.dll;
//     polls at the rate specified in ParallelPortConfig.pollRateMs.
//   All other configurations: stub (always inactive, logs a warning).
//
// Each of the 8 data bits generates its own TriggerEvent with
//   source = "parallel_port"
//   label  = "D<bit>_<RISE|FALL>"   e.g. "D3_RISE"
//   value  = 1.0 (rising) or 0.0 (falling)
//
// Bit polarity: invertLogic = false → active-high (default).
//               invertLogic = true  → active-low (common in TTL circuits).

class ParallelPortTrigger : public QObject {
    Q_OBJECT
   public:
    explicit ParallelPortTrigger(const ParallelPortConfig& config, QObject* parent = nullptr);
    ~ParallelPortTrigger() override;

    // Loads InpOut32 and starts the polling timer.
    [[nodiscard]] bool start();
    void stop();

    [[nodiscard]] bool is_active() const;
    [[nodiscard]] int events_fired() const;

    // Drives the Control register's INIT pin (bit 2, portAddr+2) high or low
    // — physically separate pins from the Data register this class reads,
    // so safe to call regardless of poll state, with no bus-contention risk.
    // Bit 2 (INIT) is used deliberately: unlike Control-register bits 0/1/3
    // (Strobe/Auto-Feed/Select-In), it is not historically hardware-inverted
    // at the DB25 connector on a standard parallel port, so "true → pin high"
    // holds without a confusing surprise. No-op if the port failed to open
    // or config().sendRecordingMarker is false — callers (TriggerManager)
    // should check the latter themselves before calling, but this method
    // stays safe to call unconditionally either way.
    void set_recording_marker(bool active);

    // The configuration this instance was constructed with — lets callers
    // (e.g. TriggerManager::start_recording()/stop_recording()) check
    // sendRecordingMarker per-instance without needing separate index
    // bookkeeping, since portTriggers only contains *enabled* ports.
    [[nodiscard]] const ParallelPortConfig& config() const;

   signals:
    void triggered(mosaic::TriggerEvent event);
    void error_occurred(QString message);

   private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
