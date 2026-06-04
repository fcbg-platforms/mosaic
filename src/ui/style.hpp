#pragma once
#include <QString>

namespace mosaic {

// Dark stylesheet applied once to QApplication so all Qt Widgets
// pick it up automatically. The QML view is unaffected (it has its own theme).
inline QString dark_stylesheet() {
    return R"(
/* ── Base ──────────────────────────────────────────────────────────────── */
QWidget {
    background-color: #0f0f1e;
    color: #c8c8e0;
    font-size: 12px;
}

/* ── Main window chrome ─────────────────────────────────────────────────── */
QMainWindow::separator { background: #1a1a35; width: 2px; height: 2px; }

QMenuBar {
    background: #0a0a18;
    border-bottom: 1px solid #252545;
    padding: 2px;
}
QMenuBar::item { padding: 4px 10px; border-radius: 4px; }
QMenuBar::item:selected { background: #1e1e40; }

QMenu {
    background: #13132a;
    border: 1px solid #252545;
    border-radius: 6px;
    padding: 4px;
}
QMenu::item { padding: 5px 24px 5px 12px; border-radius: 4px; }
QMenu::item:selected { background: #1e1e40; }
QMenu::separator { height: 1px; background: #252545; margin: 4px 8px; }

QStatusBar {
    background: #0a0a18;
    border-top: 1px solid #252545;
    color: #7878a0;
    font-size: 11px;
}

/* ── Splitter ───────────────────────────────────────────────────────────── */
QSplitter::handle { background: #1a1a35; }
QSplitter::handle:hover { background: #3a3a66; }

/* ── Tabs ───────────────────────────────────────────────────────────────── */
QTabWidget::pane {
    border: 1px solid #252545;
    border-radius: 0 6px 6px 6px;
    background: #0f0f1e;
    top: -1px;
}
QTabWidget[documentMode="true"]::pane { border-top: 1px solid #252545; border-radius: 0; }

QTabBar { background: transparent; }
QTabBar::tab {
    background: #0a0a18;
    border: 1px solid #252545;
    border-bottom: none;
    padding: 6px 14px;
    color: #6666aa;
    border-radius: 5px 5px 0 0;
    margin-right: 2px;
}
QTabBar::tab:selected {
    background: #0f0f1e;
    color: #c8c8e0;
    border-bottom-color: #0f0f1e;
}
QTabBar::tab:hover:!selected { background: #141430; color: #9999bb; }

/* ── Group boxes ────────────────────────────────────────────────────────── */
QGroupBox {
    background: #13132a;
    border: 1px solid #252545;
    border-radius: 6px;
    margin-top: 10px;
    padding: 10px 8px 8px 8px;
    font-size: 11px;
    font-weight: bold;
    color: #6666aa;
    letter-spacing: 1px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 10px;
    padding: 0 5px;
    text-transform: uppercase;
}

/* ── Input controls ─────────────────────────────────────────────────────── */
QComboBox, QSpinBox, QDoubleSpinBox, QLineEdit {
    background: #0a0a17;
    border: 1px solid #252545;
    border-radius: 4px;
    padding: 3px 7px;
    color: #c8c8e0;
    min-height: 22px;
    selection-background-color: #3a3a88;
}
QComboBox:hover, QSpinBox:hover, QDoubleSpinBox:hover, QLineEdit:hover {
    border-color: #4040aa;
}
QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QLineEdit:focus {
    border-color: #6060dd;
    background: #0d0d1f;
}
QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled {
    color: #404060;
    border-color: #1a1a33;
    background: #090914;
}

QComboBox::drop-down {
    border: none;
    width: 20px;
}
QComboBox::down-arrow {
    image: none;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-top: 5px solid #6666aa;
    width: 0; height: 0;
    margin-right: 5px;
}
QComboBox QAbstractItemView {
    background: #13132a;
    border: 1px solid #3a3a66;
    border-radius: 4px;
    selection-background-color: #2a2a55;
    outline: none;
}

QSpinBox::up-button, QDoubleSpinBox::up-button,
QSpinBox::down-button, QDoubleSpinBox::down-button {
    background: transparent;
    border: none;
    width: 16px;
}
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {
    border-left: 3px solid transparent;
    border-right: 3px solid transparent;
    border-bottom: 4px solid #6666aa;
    width: 0; height: 0;
}
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {
    border-left: 3px solid transparent;
    border-right: 3px solid transparent;
    border-top: 4px solid #6666aa;
    width: 0; height: 0;
}

/* ── Checkboxes ─────────────────────────────────────────────────────────── */
QCheckBox { spacing: 7px; color: #c8c8e0; }
QCheckBox:disabled { color: #404060; }
QCheckBox::indicator {
    width: 15px; height: 15px;
    border: 1px solid #353560;
    border-radius: 3px;
    background: #0a0a17;
}
QCheckBox::indicator:hover   { border-color: #5555bb; }
QCheckBox::indicator:checked {
    background: #4040bb;
    border-color: #6060ee;
    image: none;   /* use border trick instead of image dependency */
}
QCheckBox::indicator:checked:disabled { background: #252545; border-color: #252545; }

/* ── Buttons ────────────────────────────────────────────────────────────── */
QPushButton {
    background: #1a1a3a;
    border: 1px solid #353565;
    border-radius: 5px;
    padding: 5px 14px;
    color: #c8c8e0;
    min-height: 22px;
}
QPushButton:hover   { background: #22224a; border-color: #5555aa; }
QPushButton:pressed { background: #0e0e22; }
QPushButton:disabled { color: #404060; border-color: #1a1a33; background: #10101f; }

QPushButton[flat="true"] {
    background: transparent;
    border: none;
    padding: 3px 8px;
    color: #7878a0;
}
QPushButton[flat="true"]:hover { color: #c8c8e0; background: #1a1a35; border-radius: 4px; }

/* ── Scroll bars ────────────────────────────────────────────────────────── */
QScrollArea { border: none; background: transparent; }
QScrollBar:vertical {
    background: #0a0a17;
    width: 7px;
    border-radius: 3px;
    margin: 0;
}
QScrollBar::handle:vertical {
    background: #2a2a50;
    border-radius: 3px;
    min-height: 24px;
}
QScrollBar::handle:vertical:hover { background: #4444aa; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }

QScrollBar:horizontal {
    background: #0a0a17;
    height: 7px;
    border-radius: 3px;
}
QScrollBar::handle:horizontal {
    background: #2a2a50;
    border-radius: 3px;
    min-width: 24px;
}
QScrollBar::handle:horizontal:hover { background: #4444aa; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }

/* ── Labels ─────────────────────────────────────────────────────────────── */
QLabel { background: transparent; }
QLabel[role="section"] {
    color: #5555aa;
    font-size: 10px;
    font-weight: bold;
    letter-spacing: 1px;
    text-transform: uppercase;
}
QLabel[role="muted"] { color: #555575; font-size: 11px; }
QLabel[role="unit"]  { color: #555575; font-size: 11px; }

/* ── Separators ─────────────────────────────────────────────────────────── */
QFrame[frameShape="4"],   /* HLine */
QFrame[frameShape="5"] {  /* VLine */
    color: #1e1e3a;
    border: none;
    background: #1e1e3a;
    max-height: 1px;
}
    )";
}

} // namespace mosaic
