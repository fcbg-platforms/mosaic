#pragma once
#include <QDialog>
#include <functional>
#include <memory>

#include "auth/profile_manager.hpp"

namespace mosaic {

// Full-window login / register dialog shown before the main window.
//
// UX flow:
//   • Existing profiles appear as avatar cards — click one to select it.
//   • If the selected profile has a password a password field slides in.
//   • "Continue" logs in; "Guest session" skips auth entirely.
//   • Clicking the ＋ card switches to the Register form.
//   • The window is frameless (no OS titlebar) and Escape is disabled, so
//     the only way to leave without picking a profile is the small ✕
//     button in the top-right corner, which quits the app outright rather
//     than falling through to the main window.
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
    void showEvent(QShowEvent* event) override;     // gentle fade-in on first show
    void resizeEvent(QResizeEvent* event) override; // keep the close button pinned top-right

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
    // onComplete (if given) runs once the target page actually becomes
    // current and visible — NOT synchronously after this call returns, since
    // the switch is deferred to the fade-out animation's finished signal.
    // Needed for anything that must act on the target page itself (e.g.
    // setFocus() on one of its widgets), which would otherwise run while
    // that page is still hidden behind the stack.
    void crossfade_to_page(int index, std::function<void()> onComplete = {});
    void set_error(const QString& msg);
    void set_register_error(const QString& msg);
    void clear_error();
    void attempt_login();
    void slide_password_in(bool visible);

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
