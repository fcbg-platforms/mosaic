#include "analysis/analysis_manager.hpp"
#include "utils/logger.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQueue>

namespace mosaic {

namespace {

// A queued (or in-flight) analysis job — the full script + args are
// snapshotted at analyze_session()/run_face_mask() call time rather than
// rebuilt from Impl state at dequeue time, so a later set_model() call (e.g.
// a different UI surface sharing this AnalysisManager, or the same combo box
// changing before a queued job starts) can't silently change what an
// already-queued job runs with.
//
// env carries secrets that must NOT go through `args` (which launch() logs
// verbatim via log_info) or appear in the process's command line (visible to
// other processes via a system process list) — currently only the pyannote
// Hugging Face token (run_diarization()), passed as HF_TOKEN.
struct Job {
    QString              sessionPath;
    QString              scriptRelPath;
    QStringList          args;
    QProcessEnvironment  env;   // additive on top of the parent process's environment
};

} // namespace

struct AnalysisManager::Impl {
    bool     autoAnalyze  = false;
    QString  pythonPath;                   // empty = auto-detect
    QString  model        = "yolov8n-pose.pt";
    int      frameSkip    = 1;

    QString       currentSession;
    QProcess*     process = nullptr;
    QQueue<Job>   queue;                   // jobs waiting to be processed
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
    const QStringList args = {
        "--session",    sessionPath,
        "--model",      d->model,
        "--skip",       QString::number(d->frameSkip),
        "--out-format", "json",
    };
    enqueue_or_launch(sessionPath, "analysis/run_pose.py", args, {});
}

void AnalysisManager::run_face_mask(const QString& sessionPath, const QString& backend,
                                     const QString& style, int frameSkip) {
    const QStringList args = {
        "--session", sessionPath,
        "--backend", backend,
        "--style",   style,
        "--skip",    QString::number(qMax(1, frameSkip)),
    };
    enqueue_or_launch(sessionPath, "analysis/run_face_mask.py", args, {});
}

void AnalysisManager::run_diarization(const QString& sessionPath, const QString& modelSize,
                                       const QString& language, const QString& hfToken,
                                       int minSpeakers, int maxSpeakers, bool skipDiarization) {
    QStringList args = {"--session", sessionPath, "--model", modelSize};
    if (!language.isEmpty()) { args << "--language" << language; }
    if (minSpeakers > 0)     { args << "--min-speakers" << QString::number(minSpeakers); }
    if (maxSpeakers > 0)     { args << "--max-speakers" << QString::number(maxSpeakers); }
    if (skipDiarization)     { args << "--skip-diarization"; }

    // hfToken travels via the subprocess's environment (HF_TOKEN, already
    // read by run_diarize.py as an env-var fallback), never as a CLI arg or
    // in `args` — args gets logged verbatim by launch()'s "Starting: ..."
    // line, which would otherwise leak the token into mosaic.log.
    QProcessEnvironment env;
    if (!hfToken.isEmpty()) { env.insert("HF_TOKEN", hfToken); }

    enqueue_or_launch(sessionPath, "analysis/run_diarize.py", args, env);
}

void AnalysisManager::enqueue_or_launch(const QString& sessionPath, const QString& scriptRelPath,
                                         const QStringList& args, const QProcessEnvironment& env) {
    const Job job{sessionPath, scriptRelPath, args, env};
    if (is_running()) {
        d->queue.enqueue(job);
        log_info(QString("[AnalysisManager] Queued session: %1 (%2)")
                     .arg(sessionPath, scriptRelPath));
        return;
    }

    launch(job.sessionPath, job.scriptRelPath, job.args, job.env);
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

void AnalysisManager::launch(const QString& sessionPath, const QString& scriptRelPath,
                              const QStringList& args, const QProcessEnvironment& env) {
    const QString python = d->pythonPath.isEmpty() ? find_python() : d->pythonPath;
    if (python.isEmpty()) {
        const QString msg =
            "Python interpreter not found. "
            "Run 'bash analysis/setup.sh' to create the virtual environment.";
        log_error("[AnalysisManager] " + msg);
        emit setup_error(msg);
        return;
    }

    const QString script = find_script(scriptRelPath);
    if (script.isEmpty()) {
        const QString msg = scriptRelPath + " not found. "
            "Make sure the analysis/ directory is present next to the application.";
        log_error("[AnalysisManager] " + msg);
        emit setup_error(msg);
        return;
    }

    d->currentSession = sessionPath;
    d->process = new QProcess(this);
    d->process->setProcessChannelMode(QProcess::SeparateChannels);

    // env is additive (e.g. HF_TOKEN for run_diarize.py) on top of the full
    // parent environment — QProcess::setProcessEnvironment() REPLACES the
    // child's entire environment if called, so starting from
    // systemEnvironment() rather than the (possibly empty) `env` itself is
    // required to keep PATH and everything else the interpreter needs.
    if (!env.isEmpty()) {
        QProcessEnvironment fullEnv = QProcessEnvironment::systemEnvironment();
        for (const QString& key : env.keys()) {
            fullEnv.insert(key, env.value(key));
        }
        d->process->setProcessEnvironment(fullEnv);
    }

    connect(d->process, &QProcess::readyReadStandardOutput,
            this, &AnalysisManager::on_stdout_ready);
    connect(d->process, &QProcess::readyReadStandardError,
            this, &AnalysisManager::on_stderr_ready);
    connect(d->process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &AnalysisManager::on_process_finished);

    const QStringList fullArgs = QStringList{script} + args;

    log_info(QString("[AnalysisManager] Starting: %1 %2")
                 .arg(python, fullArgs.join(' ')));

    d->process->start(python, fullArgs);

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

    // Process any queued jobs, each with the script/args that were built at
    // enqueue time (not whatever is current now).
    if (!d->queue.isEmpty()) {
        const Job job = d->queue.dequeue();
        launch(job.sessionPath, job.scriptRelPath, job.args, job.env);
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

QString AnalysisManager::find_script(const QString& scriptRelPath) const {
    const QStringList bases = {
        QCoreApplication::applicationDirPath(),
        QCoreApplication::applicationDirPath() + "/../../../..",
        QDir::currentPath(),
    };

    for (const QString& base : bases) {
        const QString candidate = QDir(base).filePath(scriptRelPath);
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

} // namespace mosaic
