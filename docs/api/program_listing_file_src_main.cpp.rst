
.. _program_listing_file_src_main.cpp:

Program Listing for File main.cpp
=================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_main.cpp>` (``src\main.cpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #include "auth/profile_manager.hpp"
   #include "core/application.hpp"
   #include "ui/auth/admin_panel_dialog.hpp"
   #include "ui/auth/login_dialog.hpp"
   #include "ui/style.hpp"
   #include <QApplication>
   
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
   
   // Exit code returned by MainWindow when the user picks "Switch profile".
   static constexpr int k_switch_exit_code = 42;
   
   int main(int argc, char* argv[])
   {
   #ifdef _WIN32
       ::SetUnhandledExceptionFilter(mosaic_exception_filter);
   #endif
   
       QApplication app(argc, argv);
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
