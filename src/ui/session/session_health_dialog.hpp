#pragma once
#include <QDialog>

#include "session/session_health.hpp"

namespace mosaic {

// Non-modal, one-shot summary shown right after a recording stops —
// synthesizes per-camera diagnostics (frame counts/drops, GVSP packet loss,
// achieved vs. configured fps, action-command missed-trigger count, sync
// coverage) that previously only lived scattered across mosaic.log. A
// static snapshot of the report it was constructed with, not a live
// dashboard — reopen a recording session for a live view via the Real-time
// tab / Performance monitor instead. Non-modal (shown via show(), not
// exec()) so the user can still interact with the rest of the app (e.g.
// check a flagged camera's settings) while it's open.
class SessionHealthDialog : public QDialog {
    Q_OBJECT
   public:
    explicit SessionHealthDialog(const SessionHealthReport& report, QWidget* parent = nullptr);

   private:
    SessionHealthReport m_report;
};

} // namespace mosaic
