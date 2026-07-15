#include "analysis/analysis_manager.hpp"
#include "utils/logger.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QQueue>

namespace mosaic {

struct AnalysisManager::Impl {
    bool     autoAnalyze  = false;
    QString  pythonPath;                   // empty = auto-detect
    QString  model        = "yolov8n-pose.pt";
    int      frameSkip    = 1;

    QString       currentSession;
    QProcess*     process = nullptr;
    QQueue<QString> queue;                 // sessions waiting to be processed
};

AnalysisManager::AnalysisManager(QObject* parent)
    : QObject(parent), d(std::make_unique<Impl>()) {}

AnalysisManager::~AnalysisManager() { stop(); }

// ── Configuration ─────────────────────────────────────────────────────────

void AnalysisManager::set_auto_analyze(bool enabled) { d->autoAnalyze = enabled; }
bool AnalysisManager::auto_analyze()                  const { return d->autoAnalyze; }
void AnalysisManager::set_python_path(const QString& p) { d->pythonPath = p; }
void AnalysisManager::set_model(const QString& m)     { d->model = m; }
void AnalysisManager::set_frame_skip(int skip)         { d->frameSkip = qMax(1, skip); }
bool AnalysisManager::is_running()                    const { return d->process != nullptr; }

// ── Public operations ──────────────────────────────────────────────────────

void AnalysisManager::analyze_session(const QString& sessionPath) {
    if (!d->autoAnalyze) { return; }

    if (is_running()) {
        d->queue.enqueue(sessionPath);
        log_info(QString("[AnalysisManager] Queued session: %1").arg(sessionPath));
        return;
    }

    launch(sessionPath);
}

void AnalysisManager::stop() {
    if (!d->process) { return; }
    d->process->terminate();
    if (!d->process->waitForFinished(3000)) {
        d->process->kill();
    }
    d->process->deleteLater();
    d->process = nullptr;
}

// ── Launch ─────────────────────────────────────────────────────────────────

void AnalysisManager::launch(const QString& sessionPath) {
    const QString python = d->pythonPath.isEmpty() ? find_python() : d->pythonPath;
    if (python.isEmpty()) {
        const QString msg =
            "Python interpreter not found. "
            "Run 'bash analysis/setup.sh' to create the virtual environment.";
        log_error("[AnalysisManager] " + msg);
        emit setup_error(msg);
        return;
    }

    const QString script = find_script();
    if (script.isEmpty()) {
        const QString msg = "analysis/run_pose.py not found. "
            "Make sure the analysis/ directory is present next to the application.";
        log_error("[AnalysisManager] " + msg);
        emit setup_error(msg);
        return;
    }

    d->currentSession = sessionPath;
    d->process = new QProcess(this);
    d->process->setProcessChannelMode(QProcess::SeparateChannels);

    connect(d->process, &QProcess::readyReadStandardOutput,
            this, &AnalysisManager::on_stdout_ready);
    connect(d->process, &QProcess::readyReadStandardError,
            this, &AnalysisManager::on_stderr_ready);
    connect(d->process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &AnalysisManager::on_process_finished);

    const QStringList args = {
        script,
        "--session",    sessionPath,
        "--model",      d->model,
        "--skip",       QString::number(d->frameSkip),
        "--out-format", "json",
    };

    log_info(QString("[AnalysisManager] Starting: %1 %2")
                 .arg(python, args.join(' ')));

    d->process->start(python, args);

    if (!d->process->waitForStarted(5000)) {
        log_error("[AnalysisManager] Failed to start Python process: "
                  + d->process->errorString());
        emit setup_error("Failed to start analysis process: " + d->process->errorString());
        d->process->deleteLater();
        d->process = nullptr;
        return;
    }

    emit analysis_started(sessionPath);
}

// ── Process I/O slots ──────────────────────────────────────────────────────

void AnalysisManager::on_stdout_ready() {
    if (!d->process) { return; }
    while (d->process->canReadLine()) {
        const QString line = QString::fromUtf8(d->process->readLine()).trimmed();
        if (!line.isEmpty()) {
            emit output_received(line);
        }
    }
}

void AnalysisManager::on_stderr_ready() {
    if (!d->process) { return; }
    while (d->process->canReadLine()) {
        const QString line = QString::fromUtf8(d->process->readLine()).trimmed();
        if (!line.isEmpty()) {
            // Python prints progress to stderr — forward as normal output
            emit output_received("[stderr] " + line);
        }
    }
}

void AnalysisManager::on_process_finished(int exitCode, int exitStatus) {
    const bool success = (exitCode == 0)
        && (exitStatus == QProcess::NormalExit);

    log_info(QString("[AnalysisManager] Process finished — exit code %1 (%2)")
                 .arg(exitCode)
                 .arg(success ? "success" : "error"));

    emit analysis_finished(d->currentSession, success);

    if (d->process) {
        d->process->deleteLater();
        d->process = nullptr;
    }

    // Process any queued sessions
    if (!d->queue.isEmpty()) {
        launch(d->queue.dequeue());
    }
}

// ── Path discovery ─────────────────────────────────────────────────────────

QString AnalysisManager::find_venv_python() const {
    // Search relative to app executable and project root.
    const QStringList bases = {
        QCoreApplication::applicationDirPath(),
        QCoreApplication::applicationDirPath() + "/../../../..",  // Xcode bundle
        QDir::currentPath(),
    };

#ifdef Q_OS_WIN
    const QString rel = "analysis/.venv/Scripts/python.exe";
#else
    const QString rel = "analysis/.venv/bin/python";
#endif

    for (const QString& base : bases) {
        const QString candidate = QDir(base).filePath(rel);
        if (QFileInfo(candidate).isExecutable()) {
            return candidate;
        }
    }
    return {};
}

QString AnalysisManager::find_python() const {
    const QString venv = find_venv_python();
    if (!venv.isEmpty()) { return venv; }

    // Fall back to system Python
    for (const QString name : {"python3", "python"}) {
        QProcess probe;
        probe.start(name, {"--version"});
        if (probe.waitForFinished(2000) && probe.exitCode() == 0) {
            return name;
        }
    }
    return {};
}

QString AnalysisManager::find_script() const {
    const QStringList bases = {
        QCoreApplication::applicationDirPath(),
        QCoreApplication::applicationDirPath() + "/../../../..",
        QDir::currentPath(),
    };

    for (const QString& base : bases) {
        const QString candidate = QDir(base).filePath("analysis/run_pose.py");
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

} // namespace mosaic
