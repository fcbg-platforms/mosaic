#pragma once
#include "utils/logger.hpp"
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>
#include <memory>

namespace mosaic {

// Dockable log panel that subscribes to Logger::entry_added and displays
// every message in a colour-coded, auto-scrolling plain-text view.
//
// Colour scheme (matching the dark stylesheet):
//   Trace / Debug → #555575 (muted grey)
//   Info          → #c8c8e0 (default foreground)
//   Warning       → #ddaa44 (amber)
//   Error         → #dd4444 (red)
//   Critical      → #ff2222 (bright red, bold)
//
// Controls:
//   • Level filter combo  — hides messages below a threshold
//   • Clear button        — wipes the current view (does not affect the log file)
//   • Auto-scroll toggle  — pin to bottom vs. free-scroll
//   • Max-lines setting   — oldest lines are pruned to keep the widget light

class LoggerPanelW : public QWidget {
    Q_OBJECT
public:
    explicit LoggerPanelW(QWidget* parent = nullptr);
    ~LoggerPanelW() override;

    void set_max_lines(int max);

private slots:
    void on_entry_added(int level, QString timestamp, QString location, QString message);

private:
    void build_toolbar(QVBoxLayout* parent);
    void append_entry(int level, const QString& timestamp,
                       const QString& location, const QString& message);

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
