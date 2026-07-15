#pragma once
#include "trigger/trigger_types.hpp"
#include <QWidget>
#include <memory>

namespace mosaic {

class TriggerManager;

/// @brief Full-featured trigger event history panel.
///
/// Displays every TriggerEvent from all sources in a filterable, exportable
/// table.  Unlike the compact log embedded in TriggerSettingsW, this panel
/// shows structured columns (timestamp, source badge, label, action, value)
/// and supports search and CSV export.
///
/// @par Layout
/// @code
/// ┌─ Filter bar ──────────────────────────────────────────┐
/// │  [All sources ▼]  [Search label…]  [Clear]  [Export] │
/// └───────────────────────────────────────────────────────┘
/// ┌─ Events table ────────────────────────────────────────┐
/// │  Wall time  │ Elapsed  │ Source  │ Label  │ Action │ Value │
/// │  14:32:06   │  1.523 s │ [KBD]  │ Event A│ Log    │  1    │
/// │  14:32:07   │  2.100 s │ [SER]  │ S1     │ ▶ REC  │  255  │
/// └───────────────────────────────────────────────────────┘
/// @endcode
///
/// @par Auto-scroll
/// A checkbox pins the view to the most recent row.  Scrolling up
/// automatically disables auto-scroll so the user can inspect history;
/// clicking "Auto-scroll" re-enables it.
class TriggerEventPanelW : public QWidget {
    Q_OBJECT
public:
    explicit TriggerEventPanelW(TriggerManager* manager, QWidget* parent = nullptr);
    ~TriggerEventPanelW() override;

    void clear();

public slots:
    void on_event_received(const mosaic::TriggerEvent& event);

private:
    void build_filter_bar();
    void build_table();
    void apply_filter();
    void export_csv();

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
