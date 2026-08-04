#pragma once
#include "core/settings.hpp"
#include "trigger/trigger_manager.hpp"
#include <QVBoxLayout>
#include <QWidget>
#include <memory>

namespace mosaic {

class TriggerSettingsW : public QWidget {
    Q_OBJECT
public:
    explicit TriggerSettingsW(TriggerSettings& settings,
                               TriggerManager*  manager,
                               QWidget*         parent = nullptr);
    ~TriggerSettingsW() override;

signals:
    void settings_changed();

private:
    void build_master_section(QVBoxLayout* parent);
    void build_keyboard_section(QVBoxLayout* parent);
    void build_serial_section(QVBoxLayout* parent);
    void build_parallel_port_section(QVBoxLayout* parent);
    void build_event_log(QVBoxLayout* parent);

    void add_keyboard_trigger(KeyTriggerConfig cfg = {});
    void remove_keyboard_trigger(int index);
    void make_keyboard_card(int index);

    void add_serial_trigger(SerialTriggerConfig cfg = {});
    void remove_serial_trigger(int index);
    void make_serial_card(int index);

    void add_parallel_port(ParallelPortConfig cfg = {});
    void remove_parallel_port(int index);
    void make_parallel_port_card(int index);

    void on_event_received(const TriggerEvent& event);
    void reload_manager();

    TriggerSettings&  m_settings;
    TriggerManager*   m_manager;

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
