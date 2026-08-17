#include "analysis/analysis_manager.hpp"
#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QEventLoop>
#include <QStringList>
#include <QTimer>

namespace mosaic {

namespace {

// Pumps the Qt event loop until `signal` fires on `sender`, or timeoutMs
// elapses. Returns true iff the signal fired (not the timeout) -- needed
// because RUN_ALL_TESTS() runs synchronously; nothing pumps the event loop
// between test bodies otherwise, so none of AnalysisManager's real
// QProcess-driven signals (readyReadStandardOutput/Error, finished, and
// everything built on top of them) would ever actually be delivered. See
// tests/test_main.cpp for the QCoreApplication this relies on.
template <typename Sender, typename PointerToMemberSignal>
bool wait_for_signal(Sender* sender, PointerToMemberSignal signal, int timeoutMs = 5000) {
    QEventLoop loop;
    bool fired = false;
    QObject::connect(sender, signal, &loop, [&fired, &loop] { fired = true; loop.quit(); });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    return fired;
}

// tests/analysis_stub/ is built as a sibling executable in the same output
// directory as mosaic_tests itself (see tests/CMakeLists.txt) -- same
// "resolve relative to applicationDirPath()" convention AnalysisManager's
// own find_venv_python()/find_script() already rely on for the real app.
QString stub_executable_path() {
    QString path = QCoreApplication::applicationDirPath() + "/analysis_test_stub";
#if defined(Q_OS_WIN)
    path += ".exe";
#endif
    return path;
}

class AnalysisManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Every MOSAIC_TEST_STUB_* var is process-wide (qputenv has no
        // per-test scope), so start each test from a clean slate rather
        // than risk a value set by an earlier test leaking in.
        qunsetenv("MOSAIC_TEST_STUB_SLEEP_MS");
        qunsetenv("MOSAIC_TEST_STUB_ECHO_ENV");
        qunsetenv("MOSAIC_TEST_STUB_EXIT_CODE");
        mgr.set_python_path(stub_executable_path());
    }

    AnalysisManager mgr;
};

} // namespace

TEST_F(AnalysisManagerTest, StartsRealSubprocessAndEmitsAnalysisStarted) {
    // analysis_started() is emitted synchronously, in the same call stack
    // as analyze_session() itself (launch() emits it directly once
    // QProcess::waitForStarted() returns) -- unlike analysis_finished()/
    // output_received(), which only arrive later via the event loop once
    // QProcess's own async signals fire. So the listener must be connected
    // *before* calling analyze_session(), and a plain flag check is both
    // correct and sufficient here -- wait_for_signal() would connect too
    // late (the signal has already fired and gone by the time it runs).
    ASSERT_FALSE(mgr.is_running());
    bool started = false;
    QObject::connect(&mgr, &AnalysisManager::analysis_started,
                      [&started](const QString&) { started = true; });

    mgr.analyze_session("session-a");

    EXPECT_TRUE(mgr.is_running());
    EXPECT_TRUE(started);
}

TEST_F(AnalysisManagerTest, SeparatesStdoutAndStderrChannels) {
    // Direct regression test for a real bug this project shipped: stdout
    // and stderr were both read via QProcess's shared "current read
    // channel", so once stderr was read once, later stdout reads silently
    // returned stderr's data instead (see AnalysisManager::on_stdout_
    // ready()/on_stderr_ready()'s own doc comments).
    QStringList lines;
    QObject::connect(&mgr, &AnalysisManager::output_received,
                      [&lines](const QString& line) { lines << line; });

    mgr.analyze_session("session-b");
    ASSERT_TRUE(wait_for_signal(&mgr, &AnalysisManager::analysis_finished));

    EXPECT_TRUE(lines.contains("STDOUT_LINE"));
    EXPECT_TRUE(lines.contains("[stderr] STDERR_LINE"));
}

TEST_F(AnalysisManagerTest, ExitCodeZeroMeansSuccess) {
    qputenv("MOSAIC_TEST_STUB_EXIT_CODE", "0");
    bool sawSuccess = false;
    QObject::connect(&mgr, &AnalysisManager::analysis_finished,
                      [&sawSuccess](const QString&, bool success) { sawSuccess = success; });

    mgr.analyze_session("session-c");
    ASSERT_TRUE(wait_for_signal(&mgr, &AnalysisManager::analysis_finished));
    EXPECT_TRUE(sawSuccess);
}

TEST_F(AnalysisManagerTest, NonZeroExitCodeMeansFailure) {
    qputenv("MOSAIC_TEST_STUB_EXIT_CODE", "1");
    bool sawSuccess = true;
    QObject::connect(&mgr, &AnalysisManager::analysis_finished,
                      [&sawSuccess](const QString&, bool success) { sawSuccess = success; });

    mgr.analyze_session("session-d");
    ASSERT_TRUE(wait_for_signal(&mgr, &AnalysisManager::analysis_finished));
    EXPECT_FALSE(sawSuccess);
}

TEST_F(AnalysisManagerTest, EnvVarsReachTheSubprocess) {
    // Direct regression test for both run_diarization()'s HF_TOKEN
    // passthrough design and the always-forced PYTHONIOENCODING fix (see
    // AnalysisManager::launch()'s own doc comment on why the latter is
    // unconditional, not just for callers that pass extra env vars).
    qputenv("MOSAIC_TEST_STUB_ECHO_ENV", "HF_TOKEN,PYTHONIOENCODING");

    QStringList lines;
    QObject::connect(&mgr, &AnalysisManager::output_received,
                      [&lines](const QString& line) { lines << line; });

    mgr.run_diarization("session-e", "small", "", "secret-xyz", 0, 0, false);
    ASSERT_TRUE(wait_for_signal(&mgr, &AnalysisManager::analysis_finished));

    EXPECT_TRUE(lines.contains("ENV:HF_TOKEN=secret-xyz"));
    EXPECT_TRUE(lines.contains("ENV:PYTHONIOENCODING=utf-8"));
}

TEST_F(AnalysisManagerTest, QueuesSecondJobWhileFirstIsRunning) {
    qputenv("MOSAIC_TEST_STUB_SLEEP_MS", "800");

    QStringList finishedOrder;
    QObject::connect(&mgr, &AnalysisManager::analysis_finished,
                      [&finishedOrder](const QString& sessionPath, bool) {
                          finishedOrder << sessionPath;
                      });

    mgr.analyze_session("session-first");
    ASSERT_TRUE(mgr.is_running());
    mgr.analyze_session("session-second");
    // Still just the first job actually running -- the second was queued,
    // not launched concurrently, confirming enqueue_or_launch()'s "only
    // one subprocess at a time" guarantee.
    EXPECT_TRUE(mgr.is_running());

    ASSERT_TRUE(wait_for_signal(&mgr, &AnalysisManager::analysis_finished));       // first job
    ASSERT_TRUE(wait_for_signal(&mgr, &AnalysisManager::analysis_finished, 3000)); // second job

    ASSERT_EQ(finishedOrder.size(), 2);
    EXPECT_EQ(finishedOrder[0], "session-first");
    EXPECT_EQ(finishedOrder[1], "session-second");
}

TEST_F(AnalysisManagerTest, SetupErrorWhenInterpreterMissing) {
    // A non-empty but non-existent interpreter path skips launch()'s
    // "python string is empty" early-return and instead fails inside
    // QProcess::start()+waitForStarted() -- both synchronous/blocking, so
    // setup_error() fires before analyze_session() itself returns here, no
    // event-loop pump needed for this one.
    mgr.set_python_path("Z:/definitely/does/not/exist/interpreter.exe");

    QString errorMessage;
    QObject::connect(&mgr, &AnalysisManager::setup_error,
                      [&errorMessage](const QString& msg) { errorMessage = msg; });

    mgr.analyze_session("session-missing-interpreter");

    EXPECT_FALSE(mgr.is_running());
    EXPECT_FALSE(errorMessage.isEmpty());
}

} // namespace mosaic
