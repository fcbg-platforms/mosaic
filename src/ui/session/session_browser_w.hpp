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
    void launch_analysis(const QString& exe, const QStringList& args);
    [[nodiscard]] QString find_python() const;

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
