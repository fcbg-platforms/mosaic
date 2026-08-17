#pragma once
#include <QObject>
#include <QSerialPort>
#include <memory>

#include "core/settings.hpp"
#include "trigger/trigger_types.hpp"

namespace mosaic {

/// @brief Receives trigger events from an RS-232 / USB-serial port.
///
/// SerialTrigger opens the configured serial port and listens for incoming
/// bytes.  A trigger fires when the received byte satisfies the match rule
/// defined in @c SerialTriggerConfig::matchMode:
///
/// - **AnyByte** — any received byte fires.
/// - **NonZero** — fires only for non-zero bytes.
/// - **Exact**   — fires only if the byte equals @c matchValue (hex string,
///                 e.g. @c "0xFF").
///
/// The trigger label is set to the config name; the value is the decimal
/// representation of the received byte.
///
/// @par Thread safety
/// All methods must be called from the **main thread**.  QSerialPort uses
/// Qt's event loop for async I/O, so no additional thread is needed.
///
/// @see SerialTriggerConfig, TriggerManager
class SerialTrigger : public QObject {
    Q_OBJECT
   public:
    explicit SerialTrigger(SerialTriggerConfig& config, QObject* parent = nullptr);
    ~SerialTrigger() override;

    /// @brief Opens the serial port with the current config.
    /// @returns @c true on success; logs the reason on failure.
    [[nodiscard]] bool open();

    /// @brief Closes the serial port.  Safe to call when already closed.
    void close();

    /// @returns @c true while the port is open.
    [[nodiscard]] bool is_open() const;

    /// @returns The number of trigger events fired since the last reset.
    [[nodiscard]] int fire_count() const;

    void reset_count();

    /// @returns A list of all available serial port names on this system.
    [[nodiscard]] static QStringList available_ports();

   signals:
    /// Emitted on the main thread each time a matching byte is received.
    void triggered(mosaic::TriggerEvent event);

    /// Emitted whenever fire_count() changes.
    void count_changed(int count);

    /// Emitted when QSerialPort reports a non-NoError state.
    void error_occurred(QString message);

   private slots:
    void on_data_ready();
    void on_error(QSerialPort::SerialPortError error);

   private:
    [[nodiscard]] bool byte_matches(quint8 byte) const;

    SerialTriggerConfig& m_config;
    QSerialPort* m_port{nullptr};
    int m_fireCount{0};
    quint8 m_matchByte{0}; // cached parsed value for "Exact" mode
};

} // namespace mosaic
