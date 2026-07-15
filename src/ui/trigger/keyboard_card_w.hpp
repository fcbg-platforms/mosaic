#pragma once
#include "core/settings.hpp"
#include <QLabel>
#include <QWidget>
#include <memory>

namespace mosaic {

class KeyboardTrigger;

// Collapsible card for one keyboard trigger.
// Shows a key-sequence editor, name field, live fire counter, and enabled toggle.

class KeyboardCardW : public QWidget {
    Q_OBJECT
public:
    explicit KeyboardCardW(KeyTriggerConfig& config, int index,
                            QWidget* parent = nullptr);
    ~KeyboardCardW() override;

    // Connected by TriggerSettingsW to the matching KeyboardTrigger::count_changed.
    void on_count_changed(int count);

    void set_index(int index);

signals:
    void config_changed();
    void remove_requested(int index);

private:
    void build_header();
    void build_body();
    void toggle_expanded();
    void update_header_summary();

    KeyTriggerConfig& m_config;
    int   m_index;
    bool  m_expanded{true};

    QWidget* m_body{nullptr};
    QWidget* m_expandBtn{nullptr};
    QLabel*  m_nameLabel{nullptr};
    QLabel*  m_summaryLabel{nullptr};
    QLabel*  m_counterLabel{nullptr};
};

} // namespace mosaic
