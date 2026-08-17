#pragma once
#include <QLabel>
#include <QWidget>
#include <memory>

#include "core/settings.hpp"

namespace mosaic {

class SerialTrigger;

/// Collapsible settings card for one serial-port trigger.
///
/// Shows: port name combo (auto-populated from system), baud rate,
/// data bits / parity / stop bits, match mode, action combo, and a
/// real-time fire counter.  All changes write immediately into the
/// referenced @c SerialTriggerConfig and emit @c config_changed().
class SerialCardW : public QWidget {
    Q_OBJECT
   public:
    explicit SerialCardW(SerialTriggerConfig& config, int index, QWidget* parent = nullptr);
    ~SerialCardW() override;

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
    void update_enabled_states();

    SerialTriggerConfig& m_config;
    int m_index;
    bool m_expanded{true};

    QWidget* m_body{nullptr};
    QWidget* m_expandBtn{nullptr};
    QLabel* m_nameLabel{nullptr};
    QLabel* m_summaryLabel{nullptr};
    QLabel* m_counterLabel{nullptr};
};

} // namespace mosaic
