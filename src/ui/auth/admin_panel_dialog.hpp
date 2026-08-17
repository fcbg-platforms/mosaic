#pragma once
#include <QDialog>
#include <memory>

#include "auth/profile_manager.hpp"

namespace mosaic {

// Full-screen admin management panel shown after admin login, before MainWindow.
//
// Layout:
//   Left  — scrollable profile list (avatar + name + role badge + last login)
//   Right — detail/editor pane for the selected profile:
//             display name, institution, role toggle, password reset,
//             config folder, export/import, delete
//   Footer — "Enter as Admin" | "Launch as [selected user]" | "Back to Login"
//
// After exec() returns QDialog::Accepted, read selected_username() to know
// which profile to initialise the application as.  An empty string means the
// admin themselves (i.e. their own settings directory is used).

class AdminPanelDialog : public QDialog {
    Q_OBJECT
   public:
    explicit AdminPanelDialog(ProfileManager& profileMgr, const QString& adminUsername,
                              QWidget* parent = nullptr);
    ~AdminPanelDialog() override;

    // Username the admin chose to launch MOSAIC as.  May be the admin's own
    // username or any other profile's username.
    [[nodiscard]] QString selected_username() const;

   protected:
    void keyPressEvent(QKeyEvent* event) override;

   private slots:
    void on_profile_selected(const QString& username);
    void refresh_profile_list();

   private:
    void build_ui();
    void build_profile_list(QWidget* parent);
    void build_detail_pane(QWidget* parent);
    void populate_detail(const QString& username);
    void save_display_name();
    void save_institution();
    void reset_password();
    void toggle_role();
    void open_config_folder();
    void export_config();
    void import_config();
    void delete_profile();

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
