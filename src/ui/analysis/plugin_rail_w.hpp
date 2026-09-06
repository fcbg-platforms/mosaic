#pragma once
#include <QHash>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <memory>

namespace mosaic {

/// Whether a plugin's output already exists for the selected session.
///
/// Four states rather than a tick/no-tick, because two of them cannot be
/// answered yes/no honestly. Pose and rPPG namespace their output by the
/// selected model/backend, so "there is a file, but not for what you have
/// selected" is a real and common answer; and most plugins run per camera, so
/// "three of five cameras" is too. Collapsing either into "done" would be
/// confidently wrong exactly when the user is deciding whether to re-run.
enum class PluginRunState {
    Unknown,  ///< No session selected — nothing can be said.
    None,     ///< No output at all for this plugin.
    Partial,  ///< Some cameras, or output from a different model/backend.
    Complete, ///< Every camera, for the currently-selected settings.
};

/// The Analysis tab's plugin picker: a categorized, keyboard-navigable list of
/// every plugin, with a one-line description and a run-state dot per entry.
///
/// Replaces a flat 10-entry combo box. The combo gave no room to say what a
/// plugin does, no way to separate real analyses from data-prep utilities, and
/// no way to see which outputs already exist without opening all ten in turn.
///
/// Rows are plain widgets painted by hand rather than QListWidget items. That
/// is this codebase's established idiom for a selectable list with categories
/// (see SessionRow in session_browser_w.cpp and the profile list in
/// admin_panel_dialog.cpp) and it sidesteps the usual trick of faking
/// non-selectable header rows inside a selection model — headers here are just
/// labels in the layout, unreachable by the keyboard by construction.
class AnalysisPluginRailW : public QWidget {
    Q_OBJECT

   public:
    /// @param availableIds Plugin ids that actually have a controls page. A
    ///        registry entry missing from this list gets no row, so the rail
    ///        can never offer a plugin the tab cannot display.
    explicit AnalysisPluginRailW(const QStringList& availableIds, QWidget* parent = nullptr);
    ~AnalysisPluginRailW() override;

    /// Moves the highlight. Deliberately a pure state setter: it emits
    /// nothing, so the owner can call it from its own selection handler
    /// without looping back through this widget's signal.
    void set_current(const QString& pluginId);
    [[nodiscard]] QString current_plugin() const;

    /// Run-state dots. Ids absent from the map fall back to Unknown, which
    /// draws nothing at all rather than implying "not run".
    void set_run_states(const QHash<QString, PluginRunState>& states);

   signals:
    /// Emitted only for a real user selection — a click or a keypress — never
    /// from set_current().
    void plugin_selected(QString pluginId);

   private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
