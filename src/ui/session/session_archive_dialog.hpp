#pragma once
#include <QDialog>
#include <QList>
#include <memory>

#include "core/settings.hpp"
#include "session/session_info.hpp"

namespace mosaic {

/// Lists a profile's sessions with their size and age, pre-ticks the ones
/// older than RecordSettings::archiveAfterDays, and moves the ticked ones to
/// RecordSettings::archiveDirectory with move_session_verified(), one after
/// another off the UI thread. Nothing moves until Archive is clicked.
class SessionArchiveDialog : public QDialog {
    Q_OBJECT

   public:
    /// `sessions`: the profile's own sessions in `recordsDir`. `record` is
    /// edited in place (archive folder, suggestion age), and saved with the
    /// other settings when MOSAIC closes. `activeSessionPath`
    /// is the session being recorded right now, if any.
    SessionArchiveDialog(const QList<SessionInfo>& sessions, const QString& recordsDir,
                         RecordSettings* record, const QString& activeSessionPath,
                         const QString& username, QWidget* parent = nullptr);
    ~SessionArchiveDialog() override;

    /// True once at least one session was moved.
    [[nodiscard]] bool moved_any() const;

   protected:
    void reject() override;

   private:
    void build();
    void apply_suggestions();
    void refresh_space();
    void refresh_selection();
    void choose_folder();
    void start();
    void finish_one(int row, int status, const QString& message);
    void finish_all();

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
