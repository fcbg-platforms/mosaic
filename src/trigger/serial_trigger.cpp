#include "trigger/serial_trigger.hpp"

#include <QSerialPortInfo>

#include "utils/logger.hpp"
#include "utils/timestamp.hpp"

namespace mosaic {

SerialTrigger::SerialTrigger(SerialTriggerConfig& config, QObject* parent)
    : QObject(parent), m_config(config) {
    // Pre-parse the match byte so we don't do it on every received byte.
    bool ok     = false;
    m_matchByte = static_cast<quint8>(config.matchValue.toUInt(&ok, 16));
}

SerialTrigger::~SerialTrigger() { close(); }

// ── Port control ───────────────────────────────────────────────────────────

bool SerialTrigger::open() {
    if (m_port && m_port->isOpen()) {
        return true;
    }
    if (m_config.portName.isEmpty()) {
        log_warning("[SerialTrigger] Port name is empty — not opening.");
        return false;
    }

    m_port = new QSerialPort(m_config.portName, this);
    m_port->setBaudRate(m_config.baudRate);

    switch (m_config.dataBits) {
        case 5:
            m_port->setDataBits(QSerialPort::Data5);
            break;
        case 6:
            m_port->setDataBits(QSerialPort::Data6);
            break;
        case 7:
            m_port->setDataBits(QSerialPort::Data7);
            break;
        default:
            m_port->setDataBits(QSerialPort::Data8);
            break;
    }

    if (m_config.parity == "Even") {
        m_port->setParity(QSerialPort::EvenParity);
    } else if (m_config.parity == "Odd") {
        m_port->setParity(QSerialPort::OddParity);
    } else {
        m_port->setParity(QSerialPort::NoParity);
    }

    if (m_config.stopBits >= 2.0) {
        m_port->setStopBits(QSerialPort::TwoStop);
    } else if (m_config.stopBits >= 1.5) {
        m_port->setStopBits(QSerialPort::OneAndHalfStop);
    } else {
        m_port->setStopBits(QSerialPort::OneStop);
    }

    m_port->setFlowControl(QSerialPort::NoFlowControl);

    connect(m_port, &QSerialPort::readyRead, this, &SerialTrigger::on_data_ready);
    connect(m_port, &QSerialPort::errorOccurred, this, &SerialTrigger::on_error);

    if (!m_port->open(QIODevice::ReadOnly)) {
        const QString msg = QString("[SerialTrigger] Cannot open %1: %2")
                                .arg(m_config.portName, m_port->errorString());
        log_error(msg);
        emit error_occurred(msg);
        m_port->deleteLater();
        m_port = nullptr;
        return false;
    }

    log_info(QString("[SerialTrigger] Opened %1 @ %2 baud.")
                 .arg(m_config.portName)
                 .arg(m_config.baudRate));
    return true;
}

void SerialTrigger::close() {
    if (!m_port) {
        return;
    }
    if (m_port->isOpen()) {
        m_port->close();
    }
    m_port->deleteLater();
    m_port = nullptr;
    log_info(QString("[SerialTrigger] Closed %1.").arg(m_config.portName));
}

bool SerialTrigger::is_open() const { return m_port && m_port->isOpen(); }

// ── Data reception ─────────────────────────────────────────────────────────

void SerialTrigger::on_data_ready() {
    const QByteArray data = m_port->readAll();
    for (const char raw : data) {
        const auto byte = static_cast<quint8>(raw);
        if (!byte_matches(byte)) {
            continue;
        }

        ++m_fireCount;
        emit count_changed(m_fireCount);

        TriggerEvent ev;
        ev.timestampNs = elapsed_ns();
        ev.source      = "serial";
        ev.label       = m_config.name;
        ev.value       = static_cast<double>(byte);
        ev.action      = TriggerAction::Log; // filled by TriggerManager
        emit triggered(ev);
    }
}

bool SerialTrigger::byte_matches(quint8 byte) const {
    if (m_config.matchMode == "NonZero") {
        return byte != 0;
    }
    if (m_config.matchMode == "Exact") {
        return byte == m_matchByte;
    }
    return true; // AnyByte
}

void SerialTrigger::on_error(QSerialPort::SerialPortError error) {
    if (error == QSerialPort::NoError) {
        return;
    }
    const QString msg = QString("[SerialTrigger] Port error on %1: %2")
                            .arg(m_config.portName, m_port ? m_port->errorString() : "?");
    log_error(msg);
    emit error_occurred(msg);
}

// ── Utility ────────────────────────────────────────────────────────────────

int SerialTrigger::fire_count() const { return m_fireCount; }
void SerialTrigger::reset_count() {
    m_fireCount = 0;
    emit count_changed(0);
}

QStringList SerialTrigger::available_ports() {
    QStringList names;
    for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts()) {
        names << info.portName();
    }
    return names;
}

} // namespace mosaic
