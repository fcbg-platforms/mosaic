#pragma once
#include <QDialog>
#include <QString>
#include <QStringList>
#include <memory>

#include "analysis/analysis_manager.hpp"

namespace mosaic {

// Full-featured session browser and annotation editor.
//
// Shows all recorded sessions under recordsDir, renders per-session details
// (metadata, file list, analysis files) and a colour-coded annotation editor
// with JSON persistence.
//
// Analysis buttons trigger:
//   Pose   → AnalysisManager::analyze_session() (if analysisMgr != nullptr)
//   Motion → run_motion.py spawned via QProcess

class SessionBrowserW : public QDialog {
    Q_OBJECT
   public:
    // extraDirectories: additional session-root directories to scan and
    // merge alongside recordsDir — used for per-user recording access
    // control (item 27): empty for a regular user (recordsDir alone is
    // already that user's own, correctly-scoped folder), or every other
    // known profile's own recording directory when the active profile is
    // an admin, so the browser shows an aggregated, all-users view.
    explicit SessionBrowserW(const QString& recordsDir, AnalysisManager* analysisMgr = nullptr,
                             const QStringList& extraDirectories = {}, QWidget* parent = nullptr);
    ~SessionBrowserW() override;

   private:
    void build_left_panel();
    void build_right_panel();
    void rebuild_session_list();
    void apply_filter(const QString& text);
    void select_session(const QString& path);
    void populate_detail();
    void rebuild_annot_table();
    void add_annotation();
    void delete_annotation(int row);
    void save_annotations();
    void export_annot_csv();
    void run_pose_analysis();
    void run_motion_analysis();
    /// Writes the session's report (analysis/run_session_report.py) and
    /// opens it in the browser.
    void make_session_report();
    /// Combines every session's summary row into one CSV and opens it.
    void export_sessions_summary();
    /// analysis/<name> next to the app or in the source tree, or empty.
    [[nodiscard]] QString find_analysis_script(const QString& name) const;
    /// Runs one analysis script at a time; @p openWhenDone (if any) is
    /// opened when it succeeds.
    void launch_analysis(const QString& exe, const QStringList& args,
                         const QString& openWhenDone = QString());
    /// Enables or disables every button that starts a job.
    void set_job_running(bool running);
    [[nodiscard]] QString find_python() const;

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
