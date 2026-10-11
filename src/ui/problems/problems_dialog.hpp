#pragma once
#include <QDialog>
#include <memory>

namespace mosaic {

/// Help > Problems: this PC's problem journal (utils/problem_journal.hpp),
/// newest first, for the current profile (every profile for an admin), with
/// an export of the journal and logs as one .zip to send to someone.
class ProblemsDialog : public QDialog {
    Q_OBJECT

   public:
    ProblemsDialog(const QString& root, const QString& username, bool isAdmin,
                   QWidget* parent = nullptr);
    ~ProblemsDialog() override;

   private:
    void reload();
    void export_zip();

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
