#include "auth/profile_manager.hpp"
#include "core/application.hpp"
#include "ui/auth/admin_panel_dialog.hpp"
#include "ui/auth/login_dialog.hpp"
#include "ui/style.hpp"
#include "utils/logger.hpp"
#include <QApplication>
#include <exception>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

static LONG WINAPI mosaic_exception_filter(EXCEPTION_POINTERS* ep)
{
    // Write mosaic_crash.dmp next to the executable so the developer can open
    // it in Visual Studio or WinDbg to get the full thread call stacks.
    HANDLE file = ::CreateFileA("mosaic_crash.dmp",
                                GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{};
        info.ThreadId          = ::GetCurrentThreadId();
        info.ExceptionPointers = ep;
        info.ClientPointers    = FALSE;
        ::MiniDumpWriteDump(::GetCurrentProcess(),
                            ::GetCurrentProcessId(),
                            file,
                            static_cast<MINIDUMP_TYPE>(
                                MiniDumpWithThreadInfo |
                                MiniDumpWithProcessThreadData |
                                MiniDumpWithUnloadedModules),
                            &info, nullptr, nullptr);
        ::CloseHandle(file);
    }
    return EXCEPTION_CONTINUE_SEARCH;   // let the default handler terminate
}
#endif

// QApplication::notify() is the single choke point every slot invoked
// through the Qt event loop passes through (button clicks, QTimer::timeout,
// etc.) — Qt does not catch exceptions escaping a slot itself, so by
// default an uncaught exception thrown from inside any slot propagates out
// of notify() and terminates the process (this is a well-documented Qt
// hazard, not specific to this codebase). Wrapping the base call here turns
// that into a logged, recoverable error instead — the same "one bad thing
// shouldn't kill everything" philosophy already applied throughout
// VideoGrabber's own try_set() helper, just at the one place that covers
// every main-thread slot at once rather than one call site at a time.
class MosaicApplication : public QApplication {
public:
    using QApplication::QApplication;

    bool notify(QObject* receiver, QEvent* event) override {
        try {
            return QApplication::notify(receiver, event);
        } catch (const std::exception& e) {
            mosaic::log_error(QString("[MosaicApplication] Uncaught exception in a Qt slot: %1")
                                   .arg(QString::fromUtf8(e.what())));
        } catch (...) {
            mosaic::log_error("[MosaicApplication] Uncaught non-standard exception in a Qt slot.");
        }
        return false;
    }
};

// Exit code returned by MainWindow when the user picks "Switch profile".
static constexpr int k_switch_exit_code = 42;

int main(int argc, char* argv[])
{
#ifdef _WIN32
    ::SetUnhandledExceptionFilter(mosaic_exception_filter);
#endif

    MosaicApplication app(argc, argv);
    app.setApplicationName("MOSAIC");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("CSRU");
    app.setOrganizationDomain("csru.lab");
    app.setStyleSheet(mosaic::dark_stylesheet());

    mosaic::ProfileManager profileMgr;
    profileMgr.load();

    // ── Auth loop ─────────────────────────────────────────────────────────
    // Re-entered when the user picks File → Switch profile (exit code 42).
    while (true) {
        mosaic::LoginDialog loginDlg(profileMgr);
        if (loginDlg.exec() != QDialog::Accepted) {
            return 0;
        }

        QString username = loginDlg.active_username();

        // Admin profile: show the admin panel before entering the application.
        if (username != "guest") {
            const mosaic::Profile* prof = profileMgr.find(username);
            if (prof && prof->is_admin()) {
                mosaic::AdminPanelDialog adminDlg(profileMgr, username);
                if (adminDlg.exec() != QDialog::Accepted) {
                    continue;   // admin chose "Back to Login"
                }
                username = adminDlg.selected_username();
            }
        }

        mosaic::Application mosaic;
        mosaic.initialize(username);

        const int exitCode = app.exec();
        if (exitCode == k_switch_exit_code) {
            continue;   // re-show the login dialog
        }
        return exitCode;
    }
}
