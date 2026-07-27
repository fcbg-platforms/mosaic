
.. _program_listing_file_src_utils_logger.hpp:

Program Listing for File logger.hpp
===================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_utils_logger.hpp>` (``src\utils\logger.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include <QObject>
   #include <QString>
   #include <source_location>
   #include <memory>
   
   namespace mosaic {
   
   /// @brief Severity levels for log messages.
   ///
   /// Levels are ordered from least to most severe.  The minimum level displayed
   /// or written to disk can be set via Logger::set_min_level().
   enum class LogLevel : int {
       Trace    = 0, ///< Very detailed tracing — disabled in release builds.
       Debug    = 1, ///< Developer-facing diagnostics.
       Info     = 2, ///< Normal operational events (session start, file paths, …).
       Warning  = 3, ///< Recoverable issues (dropped frame, device fallback, …).
       Error    = 4, ///< Non-fatal errors — operation failed but the app continues.
       Critical = 5, ///< Fatal errors — the application cannot continue.
   };
   
   /// @returns A short uppercase C-string label for the given level, e.g. @c "INFO".
   [[nodiscard]] const char* log_level_label(LogLevel l) noexcept;
   
   /// @brief Thread-safe application logger with file output and a Qt signal.
   ///
   /// Logger is a **singleton** — access it via Logger::instance().  In practice,
   /// prefer the free functions (log_info, log_error, …) which automatically
   /// capture the call site via @c std::source_location.
   ///
   /// @par Connecting the UI panel
   /// @code{.cpp}
   /// // In LoggerPanelW constructor (main thread):
   /// connect(&Logger::instance(), &Logger::entry_added,
   ///         this, &LoggerPanelW::on_entry_added,
   ///         Qt::QueuedConnection);   // ← always use QueuedConnection
   /// @endcode
   ///
   /// @note The entry_added() signal is emitted from the **calling thread**.
   ///       Always use Qt::QueuedConnection when connecting to a main-thread
   ///       slot from a background worker.
   class Logger : public QObject {
       Q_OBJECT
   public:
       /// @returns The process-wide Logger singleton.
       static Logger& instance();
   
       ~Logger() override;
   
       /// @brief Logs a message at the given severity level.
       ///
       /// Thread-safe.  If a log file is open, the message is written to it
       /// (mutex-protected).  entry_added() is then emitted from the calling
       /// thread.
       ///
       /// @param level    Severity level.
       /// @param message  UTF-8 message string.
       /// @param loc      Automatically-captured source location — do not pass manually.
       void log(LogLevel level, const QString& message,
                std::source_location loc = std::source_location::current());
   
       /// @brief Sets the minimum level written to file and emitted via signal.
       ///
       /// Messages below this level are discarded.  Default: LogLevel::Trace.
       ///
       /// @param level  New minimum level.
       void set_min_level(LogLevel level);
   
       /// @brief Opens a log file for writing (appending if it already exists).
       ///
       /// @param path  Absolute file path.
       /// @returns     @c true on success.
       bool open_log_file(const QString& path);
   
       /// @brief Flushes and closes the log file.
       void close_log_file();
   
   signals:
       /// Emitted for every accepted log entry, from the calling thread.
       ///
       /// @param level      Severity as @c int — cast back with
       ///                   @c static_cast<LogLevel>(level).
       /// @param timestamp  Wall-clock string, e.g. @c "14:32:05.123".
       /// @param location   Source location, e.g. @c "record_manager.cpp:88".
       /// @param message    The log message.
       void entry_added(int level, QString timestamp, QString location, QString message);
   
   private:
       explicit Logger();
       struct Impl;
       std::unique_ptr<Impl> d;
   };
   
   // ── Convenience free functions ─────────────────────────────────────────────
   
   /// @brief Logs at Trace level.  @p loc is captured automatically.
   void log_trace   (const QString& msg, std::source_location loc = std::source_location::current());
   /// @brief Logs at Debug level.  @p loc is captured automatically.
   void log_debug   (const QString& msg, std::source_location loc = std::source_location::current());
   /// @brief Logs at Info level.  @p loc is captured automatically.
   void log_info    (const QString& msg, std::source_location loc = std::source_location::current());
   /// @brief Logs at Warning level.  @p loc is captured automatically.
   void log_warning (const QString& msg, std::source_location loc = std::source_location::current());
   /// @brief Logs at Error level.  @p loc is captured automatically.
   void log_error   (const QString& msg, std::source_location loc = std::source_location::current());
   /// @brief Logs at Critical level.  @p loc is captured automatically.
   void log_critical(const QString& msg, std::source_location loc = std::source_location::current());
   
   } // namespace mosaic
