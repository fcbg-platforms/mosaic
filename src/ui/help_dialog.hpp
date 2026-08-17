#pragma once
#include <QDialog>

namespace mosaic {

// "About / Help" dialog shown from MainWindow's Help menu — replaces the
// previous single QMessageBox::about() call with a proper window covering
// what a user actually wants to know: what the app is, its key
// capabilities, the keyboard shortcuts already defined in the menu bar
// (previously undocumented anywhere in the UI), and links to the docs/repo.
class HelpDialog : public QDialog {
    Q_OBJECT
   public:
    explicit HelpDialog(QWidget* parent = nullptr);
};

} // namespace mosaic
