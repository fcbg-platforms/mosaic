
.. _program_listing_file_src_main.cpp:

Program Listing for File main.cpp
=================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_main.cpp>` (``src/main.cpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #include "auth/profile_manager.hpp"
   #include "core/application.hpp"
   #include "ui/auth/login_dialog.hpp"
   #include "ui/style.hpp"
   #include <QApplication>
   
   // Exit code returned by MainWindow when the user picks "Switch profile".
   static constexpr int k_switch_exit_code = 42;
   
   int main(int argc, char* argv[])
   {
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
   
           const QString username = loginDlg.active_username();
   
           mosaic::Application mosaic;
           mosaic.initialize(username);
   
           const int exitCode = app.exec();
           if (exitCode == k_switch_exit_code) {
               continue;   // re-show the login dialog
           }
           return exitCode;
       }
   }
