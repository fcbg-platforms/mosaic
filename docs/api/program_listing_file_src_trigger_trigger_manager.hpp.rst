
.. _program_listing_file_src_trigger_trigger_manager.hpp:

Program Listing for File trigger_manager.hpp
============================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_trigger_trigger_manager.hpp>` (``src/trigger/trigger_manager.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include "core/settings.hpp"
   #include "trigger/trigger_types.hpp"
   #include <QObject>
   #include <memory>
   
   namespace mosaic {
   
   /// @brief Central trigger coordinator — aggregates events from all sources
   ///        into a single signal and delegates to the CSV recorder.
   ///
   /// TriggerManager is owned by Application for the full application lifetime.
   /// It manages:
   /// - **Keyboard triggers** — application-level event filters that fire on
   ///   configured key sequences.
   /// - **LSL inlets** — background threads receiving string markers from
   ///   external LSL streams (EEG amplifiers, eye-trackers, etc.)
   /// - **LSL outlet** — publishes one sample per video frame so external tools
   ///   can align to MOSAIC's camera timeline.
   /// - **Parallel port triggers** — InpOut32-based bit-edge detection (Windows).
   /// - **TriggerRecorder** — writes every TriggerEvent to @c trigger.csv.
   ///
   /// Call reload() whenever TriggerSettings change (e.g. the user edits key
   /// bindings in the UI) to rebuild all sources from the current settings.
   ///
   /// @par Thread safety
   /// event_received() and on_trigger_fired() are always invoked on the **main
   /// thread** (keyboard filters and LSL inlets use Qt::QueuedConnection).
   /// publish_frame_marker() is safe to call from **any thread**.
   ///
   /// @see TriggerEvent, KeyboardTrigger, LslOutlet, LslInlet, ParallelPortTrigger
   class TriggerManager : public QObject {
       Q_OBJECT
   public:
       /// @param settings  Trigger settings (key bindings, LSL config, parallel ports).
       ///                  Held by reference — must outlive this object.
       /// @param parent    Qt parent object.
       explicit TriggerManager(TriggerSettings& settings, QObject* parent = nullptr);
       ~TriggerManager() override;
   
       /// @brief Rebuilds all trigger sources to match the current TriggerSettings.
       ///
       /// Tears down existing sources, then re-creates keyboard triggers, LSL
       /// inlets/outlet, and parallel-port pollers from the current state of
       /// TriggerSettings.  Call this after the user changes key bindings or LSL
       /// configuration in the settings UI.
       void reload();
   
       /// @brief Opens the CSV output file and starts recording trigger events.
       ///
       /// Also sends a @c SESSION_START marker to the LSL event outlet.
       ///
       /// @param csvPath  Absolute path for the output @c trigger.csv.
       void start_recording(const QString& csvPath);
   
       /// @brief Flushes and closes the CSV file.
       ///
       /// Also sends a @c SESSION_STOP marker to the LSL event outlet.
       void stop_recording();
   
       /// @returns @c true while the CSV output file is open.
       [[nodiscard]] bool is_recording() const;
   
       /// @brief Publishes a video-frame timing marker to the LSL outlet.
       ///
       /// Call this from the VideoGrabber thread each time a frame is captured.
       /// This method is thread-safe — the underlying liblsl push is lock-free.
       ///
       /// @param cameraIndex  Camera index (0-based).
       /// @param frameId      Monotonic frame counter for this camera.
       /// @param elapsedNs    Value of elapsed_ns() at grab time.
       void publish_frame_marker(int cameraIndex, int64_t frameId, int64_t elapsedNs);
   
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
       /// @param event  A copy of the TriggerEvent (source, label, value, timestamp).
       void event_received(mosaic::TriggerEvent event);
   
   private slots:
       void on_trigger_fired(mosaic::TriggerEvent event);
   
   private:
       struct Impl;
       std::unique_ptr<Impl> d;
   };
   
   } // namespace mosaic
