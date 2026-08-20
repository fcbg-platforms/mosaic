#pragma once
#include <QObject>
#include <memory>

#include "core/settings.hpp"
#include "trigger/trigger_types.hpp"

namespace mosaic {

/// @brief Central trigger coordinator — aggregates events from all sources
///        into a single signal and delegates to the CSV recorder.
///
/// TriggerManager is owned by Application for the full application lifetime.
/// It manages:
/// - **Keyboard triggers** — application-level event filters that fire on
///   configured key sequences.
/// - **Parallel port triggers** — InpOut32-based bit-edge detection (Windows),
///   receiving external pulses (e.g. an EEG amplifier's trigger-out cable),
///   and optionally sending a recording start/stop marker back out on the
///   same port's Control register (see ParallelPortConfig::sendRecordingMarker).
/// - **TriggerRecorder** — writes every TriggerEvent to @c trigger.csv.
///
/// Call reload() whenever TriggerSettings change (e.g. the user edits key
/// bindings in the UI) to rebuild all sources from the current settings.
///
/// @par Thread safety
/// event_received() and on_trigger_fired() are always invoked on the **main
/// thread** (keyboard filters use Qt::QueuedConnection).
///
/// @see TriggerEvent, KeyboardTrigger, ParallelPortTrigger
class TriggerManager : public QObject {
    Q_OBJECT
   public:
    /// @param settings  Trigger settings (key bindings, serial/parallel ports).
    ///                  Held by reference — must outlive this object.
    /// @param parent    Qt parent object.
    explicit TriggerManager(TriggerSettings& settings, QObject* parent = nullptr);
    ~TriggerManager() override;

    /// @brief Rebuilds all trigger sources to match the current TriggerSettings.
    ///
    /// Tears down existing sources, then re-creates keyboard triggers and
    /// parallel-port pollers from the current state of TriggerSettings. Call
    /// this after the user changes key bindings or parallel-port
    /// configuration in the settings UI.
    void reload();

    /// @brief Opens the CSV output file and starts recording trigger events.
    ///
    /// Also drives high any parallel port configured with
    /// ParallelPortConfig::sendRecordingMarker, so an EEG amplifier (or any
    /// other device listening on that port's Control-register INIT pin) sees
    /// a rising edge marking the start of this recording.
    ///
    /// @param csvPath  Absolute path for the output @c trigger.csv.
    void start_recording(const QString& csvPath);

    /// @brief Flushes and closes the CSV file.
    ///
    /// Also drives low any parallel port configured with
    /// ParallelPortConfig::sendRecordingMarker (falling edge marking the end
    /// of this recording).
    void stop_recording();

    /// @returns @c true while the CSV output file is open.
    [[nodiscard]] bool is_recording() const;

    /// @returns The number of active keyboard trigger event filters.
    [[nodiscard]] int keyboard_trigger_count() const;

    /// @param index  Zero-based index.
    /// @returns      The KeyboardTrigger at @p index as a @c QObject* so the
    ///               settings panel can connect to its @c count_changed() signal
    ///               without including the private header.  Returns @c nullptr
    ///               if @p index is out of range.
    [[nodiscard]] QObject* keyboard_trigger_at(int index) const;

   signals:
    /// Emitted on the main thread for every trigger event, regardless of source.
    /// Connect to this signal to react to events in real time.
    ///
    /// @param event  A copy of the TriggerEvent (source, label, value, timestamp, action).
    void event_received(mosaic::TriggerEvent event);

    /// Emitted when a trigger fires with action StartRecording or StopRecording.
    /// Application connects this to RecordManager::start() / stop().
    ///
    /// @param action  The action requested (Start or Stop).
    /// @param event   The trigger event that caused the action.
    void action_requested(mosaic::TriggerAction action, mosaic::TriggerEvent event);

   private slots:
    void on_trigger_fired(mosaic::TriggerEvent event);

   private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
