#pragma once
#include <QString>
#include <cstdint>
#include <memory>

namespace mosaic {

// Records every GigE Action Command trigger tick of a recording, one row per
// tick, to video/action_ticks.csv:
//
//   tick,elapsed_ns,fired
//
// `elapsed_ns` is taken immediately before the broadcast, on the same
// elapsed_ns() clock as every frame's timestamp, so a frame can be placed on
// the tick that produced it (analysis/sync_repair/tick_alignment.py). `fired`
// is how many of the group's broadcasts succeeded that tick.
//
// Why it exists: the ticks used to be recorded nowhere — only a count — so
// aligning cameras afterwards had to guess the trigger grid from host arrival
// times, which carry transfer and scheduling jitter, at a rate measured after
// the fact. The command is fire-immediately (no scheduled ActionTime), so the
// moment it was sent is the only record of when each frame was triggered.
//
// Written from the ticker's own thread only (open() and close() may be called
// from another thread before the thread starts and after it has finished).
// Flushed at most once a second, so a crash leaves a file complete to within
// about a second — the same promise as the timestamp CSVs.
//
// QtCore-only, so mosaic_tests covers it.
class ActionTickLog {
   public:
    ActionTickLog();
    ~ActionTickLog();

    ActionTickLog(const ActionTickLog&)            = delete;
    ActionTickLog& operator=(const ActionTickLog&) = delete;

    /// Creates the file and writes the header. False (and logs) on failure,
    /// after which append() is a no-op — a missing tick log degrades
    /// alignment to arrival time, it must never stop a recording.
    bool open(const QString& path);

    void append(int64_t tick, int64_t elapsedNs, int fired);

    /// Flushes and closes. Safe to call twice.
    void close();

    [[nodiscard]] bool is_open() const;
    [[nodiscard]] int64_t rows_written() const;

    /// How long append() may hold rows before flushing. Default 1000 ms.
    void set_flush_interval_ms(int ms);

   private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
