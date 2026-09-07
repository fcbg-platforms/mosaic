#pragma once
#include <QDialog>
#include <QString>
#include <functional>

#include "session/session_name.hpp"

class QLabel;
class QLineEdit;
class QPushButton;

namespace mosaic {

// Asks who and what a recording is of, at the moment Record is pressed.
//
// Opened only when the question actually needs asking — the identity cannot
// produce a folder (no subject, or a name too long for the recordings
// directory), or this subject/session/task has been recorded before. With a
// usable, unrepeated identity already typed into the monitor's inline bar,
// Record starts the countdown and this never appears.
//
// It exists because the inline bar is findable in principle and missable in
// practice: it sits at the bottom of a column the operator can drag arbitrarily
// small, and every recording made on this machine before it was added carried a
// timestamp-only name. A field nobody fills is worse than no field, because the
// data still lands — just unattributable.
//
// Modal and stack-scoped by design. It is opened with exec() from a queued
// connection in MainWindow, and the caller must let it destruct before calling
// back into MonitorBridge: RecordManager::start() can block the GUI thread for
// several hundred ms, and with a zero start delay that would happen with this
// window still on screen.
class SessionIdentityDialog : public QDialog {
    Q_OBJECT
   public:
    // `advise` is the caller's judgement function, invoked on every keystroke.
    // Passed in rather than reached for so this class never learns about
    // AppSettings or the recordings directory: it renders an IdentityAdvice and
    // knows nothing about how one is arrived at.
    using AdviseFn = std::function<IdentityAdvice(const SessionIdentity&)>;

    SessionIdentityDialog(const QString& subject, const QString& session, const QString& task,
                          AdviseFn advise, QWidget* parent = nullptr);

    [[nodiscard]] QString subject() const;
    [[nodiscard]] QString session() const;
    [[nodiscard]] QString task() const;

   private:
    // Re-judges what is typed and repaints the preview, the warning and the
    // Start button's enabled state. The single place this dialog decides
    // anything — and it decides nothing itself, it only renders the advice.
    void refresh();

    AdviseFn m_advise;
    QLineEdit* m_subject = nullptr;
    QLineEdit* m_session = nullptr;
    QLineEdit* m_task    = nullptr;
    QLabel* m_preview    = nullptr;
    QLabel* m_warning    = nullptr;
    QPushButton* m_start = nullptr;
};

} // namespace mosaic
