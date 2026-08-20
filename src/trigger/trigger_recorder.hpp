#pragma once
#include <QString>
#include <memory>

#include "trigger/trigger_types.hpp"

namespace mosaic {

// Writes TriggerEvents to a CSV file during a recording session.
// Thread-safe: record_event() can be called from any thread.

class TriggerRecorder {
   public:
    TriggerRecorder();
    ~TriggerRecorder();

    // Opens the CSV file. Returns false on I/O error.
    [[nodiscard]] bool start(const QString& path);

    // Writes one event row. Safe to call from any thread.
    void record_event(const TriggerEvent& event);

    // Flushes and closes the file.
    void stop();

    [[nodiscard]] bool is_recording() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
