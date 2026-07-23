#pragma once
#include "auth/profile_manager.hpp"
#include <QDialog>
#include <memory>

namespace mosaic {

// Full-window login / register dialog shown before the main window.
//
// UX flow:
//   • Existing profiles appear as avatar cards — click one to select it.
//   • If the selected profile has a password a password field slides in.
//   • "Continue" logs in; "Guest session" skips auth entirely.
//   • Clicking the ＋ card switches to the Register form.
//   • Escape and the window close button are disabled — the user must
//     choose a session before the main window opens.
//
// After exec() returns QDialog::Accepted, read active_username() to
// find which profile was chosen ("guest" = guest session).

class LoginDialog : public QDialog {
    Q_OBJECT
public:
    explicit LoginDialog(ProfileManager& profileMgr, QWidget* parent = nullptr);
    ~LoginDialog() override;

    [[nodiscard]] QString active_username() const;

protected:
    void keyPressEvent(QKeyEvent* event) override;  // disable Escape
    void showEvent(QShowEvent* event) override;      // gentle fade-in on first show

private slots:
    void on_card_selected(const QString& username);
    void on_login_clicked();
    void on_guest_clicked();
    void on_register_clicked();
    void on_cancel_register_clicked();
    void on_password_return_pressed();

private:
    void build_ui();
    void rebuild_profile_grid();
    void show_login_mode();
    void show_register_mode();
    void crossfade_to_page(int index);
    void set_error(const QString& msg);
    void set_register_error(const QString& msg);
    void clear_error();
    void attempt_login();
    void slide_password_in(bool visible);

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
