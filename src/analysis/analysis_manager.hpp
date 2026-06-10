#pragma once
#include <QObject>
#include <QString>
#include <memory>

namespace mosaic {

/// @brief Manages the Python analysis subprocess for post-recording pose estimation.
///
/// When @ref auto_analyze is enabled and a recording session ends,
/// AnalysisManager launches the analysis/run_pose.py script in a
/// background QProcess, streaming its output to @ref output_received().
///
/// The Python script is found relative to the application's executable directory
/// (for installed builds) or the project root (during development).
///
/// @par Usage
/// @code{.cpp}
/// AnalysisManager mgr;
/// connect(&mgr, &AnalysisManager::output_received, this, [](const QString& line) {
///     log_info("[Analysis] " + line);
/// });
/// connect(recordMgr, &RecordManager::recording_stopped, &mgr,
///         [&](const QString& path, int) { mgr.analyze_session(path); });
/// mgr.set_auto_analyze(true);
/// @endcode
///
/// @see RecordManager
class AnalysisManager : public QObject {
    Q_OBJECT
public:
    explicit AnalysisManager(QObject* parent = nullptr);
    ~AnalysisManager() override;

    // ── Configuration ───────────────────────────────────────────────────────

    /// @brief Enable / disable automatic post-recording analysis.
    ///
    /// When @c true, @ref analyze_session() is triggered automatically by
    /// the @c recording_stopped connection in Application.
    void set_auto_analyze(bool enabled);

    /// @returns @c true if auto-analysis is enabled.
    [[nodiscard]] bool auto_analyze() const;

    /// @brief Override the Python interpreter path.
    ///
    /// By default, the manager searches for a virtual-environment interpreter at
    /// @c analysis/.venv/bin/python (macOS/Linux) or @c analysis\.venv\Scripts\python.exe
    /// (Windows), then falls back to the system @c python3 / @c python.
    ///
    /// @param path  Absolute path to the Python executable.
    void set_python_path(const QString& path);

    /// @brief Set the YOLOv8 model variant.
    ///
    /// @param modelName  e.g. @c "yolov8n-pose.pt" (default) or @c "yolov8s-pose.pt".
    void set_model(const QString& modelName);

    /// @brief Set how many frames to skip between pose estimates.
    ///
    /// @c 1 = every frame (slowest, most detailed).
    /// @c 5 = every 5th frame (≈6 fps at 30 fps recording).
    void set_frame_skip(int skip);

    // ── Status ───────────────────────────────────────────────────────────────

    /// @returns @c true while the analysis subprocess is running.
    [[nodiscard]] bool is_running() const;

    // ── Operations ───────────────────────────────────────────────────────────

    /// @brief Analyse all .mp4 files in @p sessionPath asynchronously.
    ///
    /// If auto_analyze() is @c false, this method is a no-op.
    /// If a previous analysis is still running, this queues the new session.
    ///
    /// @param sessionPath  Absolute path to the recorded session directory.
    void analyze_session(const QString& sessionPath);

    /// @brief Stop the currently running analysis process immediately.
    void stop();

signals:
    /// Emitted for every line of stdout / stderr from the Python subprocess.
    void output_received(QString line);

    /// Emitted when the subprocess starts.
    void analysis_started(QString sessionPath);

    /// Emitted when the subprocess exits.
    void analysis_finished(QString sessionPath, bool success);

    /// Emitted if the Python interpreter or script cannot be found.
    void setup_error(QString message);

private slots:
    void on_stdout_ready();
    void on_stderr_ready();
    void on_process_finished(int exitCode, int exitStatus);

private:
    [[nodiscard]] QString find_python()     const;
    [[nodiscard]] QString find_script()     const;
    [[nodiscard]] QString find_venv_python() const;
    void launch(const QString& sessionPath);

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
