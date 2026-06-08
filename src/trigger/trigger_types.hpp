#pragma once
#include <QString>
#include <cstdint>

namespace mosaic {

/// @brief The action a trigger performs when it fires.
///
/// Every trigger configuration (keyboard, serial, parallel port, LSL inlet)
/// carries one of these values.  The TriggerManager dispatches an
/// @c action_requested() signal for Start/Stop; @c Log is the default
/// "record and display" behaviour.
enum class TriggerAction : uint8_t {
    Log = 0,        ///< Write to CSV, display in the event panel — no control action.
    StartRecording, ///< Begin a recording session (and log the event).
    StopRecording,  ///< End the current recording session (and log the event).
};

/// @brief Returns a short human-readable label for a TriggerAction.
[[nodiscard]] inline QString trigger_action_label(TriggerAction a) noexcept {
    switch (a) {
    case TriggerAction::Log:            return "Log";
    case TriggerAction::StartRecording: return "Start Recording";
    case TriggerAction::StopRecording:  return "Stop Recording";
    }
    return "Log";
}

/// @brief Parses a label string back to TriggerAction (case-insensitive).
[[nodiscard]] inline TriggerAction trigger_action_from_label(const QString& s) noexcept {
    if (s == "Start Recording") return TriggerAction::StartRecording;
    if (s == "Stop Recording")  return TriggerAction::StopRecording;
    return TriggerAction::Log;
}



/// @brief A single timestamped event from any trigger source.
///
/// TriggerEvent is the common currency passed between trigger sources
/// (KeyboardTrigger, LslInlet, ParallelPortTrigger), TriggerManager,
/// and TriggerRecorder.  It is also emitted by TriggerManager::event_received()
/// so the UI or any downstream consumer can react in real time.
///
/// @par CSV columns
/// TriggerRecorder writes one row per event:
/// @code
/// elapsed_ms,wall_clock,source,label,value
/// 1523,14:32:06.645,keyboard,Event A,0
/// 2100,14:32:07.222,lsl,Stimulus/S1,1
/// 4910,14:32:09.032,parallel_port,D3_RISE,1
/// @endcode
struct TriggerEvent {
    /// Monotonic nanosecond timestamp from elapsed_ns() at the moment the
    /// event was detected.  Use this to align to @c timestamps_camN.csv.
    int64_t timestampNs = 0;

    /// Where the event originated.  One of:
    /// - @c "keyboard"       — a KeyboardTrigger event filter.
    /// - @c "lsl"            — a received LSL marker.
    /// - @c "parallel_port"  — a bit-edge on the LPT data register.
    QString source;

    /// Human-readable event name.
    /// - Keyboard: the user-configured binding name, e.g. @c "Event A".
    /// - LSL:      the received string sample, e.g. @c "Stimulus/S1".
    /// - Parallel: bit and edge, e.g. @c "D3_RISE" or @c "D3_FALL".
    QString label;

    /// Optional numeric payload.
    /// - Keyboard / parallel rising edge: @c 1.0.
    /// - Parallel falling edge:           @c 0.0.
    /// - LSL:                             @c 0.0 (string-only inlet).
    double  value = 0.0;

    /// The action dispatched by this event.  Set by TriggerManager when routing.
    TriggerAction action = TriggerAction::Log;
};

} // namespace mosaic
