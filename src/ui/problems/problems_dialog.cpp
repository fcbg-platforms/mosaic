#include "ui/problems/problems_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

#include "utils/problem_journal.hpp"

namespace mosaic {

struct ProblemsDialog::Impl {
    QString root;
    QString username;
    bool isAdmin = false;
    QList<ProblemEntry> all;     // this profile's (or every) entry, newest first
    QList<ProblemEntry> visible; // after the filters
    QTimer* debounce = nullptr;

    QComboBox* levelCombo = nullptr;
    QLineEdit* search     = nullptr;
    QTableWidget* table   = nullptr;
    QLabel* countLbl      = nullptr;
    QCheckBox* dumpsBox   = nullptr;
    QLabel* statusLbl     = nullptr;
};

ProblemsDialog::ProblemsDialog(const QString& root, const QString& username, bool isAdmin,
                               QWidget* parent)
    : QDialog(parent), d(std::make_unique<Impl>()) {
    d->root     = root;
    d->username = username;
    d->isAdmin  = isAdmin;
    setWindowTitle("Problems on this PC");
    resize(980, 560);

    auto* box   = new QVBoxLayout(this);
    auto* intro = new QLabel(
        isAdmin ? "Warnings, errors and unexpected closes on this PC, for every profile, kept "
                  "across restarts."
                : "Warnings, errors and unexpected closes from your profile on this PC, kept "
                  "across restarts.");
    intro->setWordWrap(true);
    box->addWidget(intro);

    auto* filters = new QHBoxLayout;
    d->levelCombo = new QComboBox;
    d->levelCombo->addItem("Errors and unexpected closes", "errors");
    d->levelCombo->addItem("Warnings too", "all");
    d->search = new QLineEdit;
    d->search->setPlaceholderText("Filter by text…");
    d->search->setClearButtonEnabled(true);
    d->countLbl = new QLabel;
    filters->addWidget(d->levelCombo);
    filters->addWidget(d->search, 1);
    filters->addWidget(d->countLbl);
    box->addLayout(filters);

    d->table = new QTableWidget(0, 4);
    d->table->setHorizontalHeaderLabels({"Time", "Level", "Profile", "Message"});
    d->table->verticalHeader()->setVisible(false);
    d->table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    d->table->setSelectionBehavior(QAbstractItemView::SelectRows);
    d->table->setWordWrap(true);
    d->table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    d->table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    d->table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    d->table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    box->addWidget(d->table, 1);

    auto* footer = new QHBoxLayout;
    d->dumpsBox  = new QCheckBox("Include crash dumps");
    d->dumpsBox->setToolTip(
        "Crash dumps help a developer find where MOSAIC crashed, but hold a copy of its memory "
        "at that moment (which can include participant names). Admins only.");
    d->dumpsBox->setVisible(isAdmin);
    auto* exportBtn = new QPushButton("Export…");
    exportBtn->setToolTip(
        "Save the entries shown here, with the logs, as one .zip to send to someone.");
    auto* folderBtn = new QPushButton("Open folder");
    auto* closeBtn  = new QPushButton("Close");
    d->statusLbl    = new QLabel;
    d->statusLbl->setWordWrap(true);
    footer->addWidget(d->dumpsBox);
    footer->addWidget(d->statusLbl, 1);
    footer->addWidget(exportBtn);
    footer->addWidget(folderBtn);
    footer->addWidget(closeBtn);
    box->addLayout(footer);

    connect(d->levelCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &ProblemsDialog::reload);
    // Filtering runs on the entries read once below; typing waits a moment.
    d->debounce = new QTimer(this);
    d->debounce->setSingleShot(true);
    d->debounce->setInterval(200);
    connect(d->debounce, &QTimer::timeout, this, &ProblemsDialog::reload);
    connect(d->search, &QLineEdit::textChanged, d->debounce, qOverload<>(&QTimer::start));
    connect(exportBtn, &QPushButton::clicked, this, &ProblemsDialog::export_zip);
    // An admin's is the whole app-data folder; anyone else's, their own
    // profile's folder (the guest's logs sit at the top).
    connect(folderBtn, &QPushButton::clicked, this, [this] {
        const QString dir = d->isAdmin || d->username == QLatin1String("guest")
                                ? d->root
                                : d->root + "/profiles/" + d->username;
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    const ProblemJournal journal(d->root);
    d->all = problems_visible_to(journal.read_all(), d->username, d->isAdmin);
    std::reverse(d->all.begin(), d->all.end()); // newest first
    reload();
}

ProblemsDialog::~ProblemsDialog() = default;

// Most rows the list shows at once (newest first); the export holds them all.
static constexpr int kMaxRows = 2000;

void ProblemsDialog::reload() {
    const bool warnings = d->levelCombo->currentData().toString() == "all";
    const QString text  = d->search->text().trimmed();
    d->visible.clear();
    for (const auto& e : d->all) {
        if (!warnings && e.level == QLatin1String("WARN")) {
            continue;
        }
        if (!text.isEmpty() && !e.message.contains(text, Qt::CaseInsensitive) &&
            !e.source.contains(text, Qt::CaseInsensitive)) {
            continue;
        }
        d->visible << e;
    }

    const int rows = std::min<int>(d->visible.size(), kMaxRows);
    d->table->setRowCount(rows);
    for (int r = 0; r < rows; ++r) {
        const auto& e      = d->visible[r];
        const QString when = e.timeUtc.toLocalTime().toString("yyyy-MM-dd hh:mm:ss");
        QString message    = e.message;
        if (e.repeat > 1) {
            message += QString("  (%1th time in that run)").arg(e.repeat);
        }
        const QString level = e.kind == QLatin1String("unclean_exit") ? "CLOSED" : e.level;
        const QStringList cells{when, level, e.profile, message};
        for (int c = 0; c < cells.size(); ++c) {
            auto* item = new QTableWidgetItem(cells[c]);
            if (c == 3) {
                item->setToolTip(
                    e.source.isEmpty()
                        ? e.message
                        : QString("%1\n\n%2, MOSAIC %3").arg(e.message, e.source, e.version));
            }
            d->table->setItem(r, c, item);
        }
    }
    if (rows <= 300) {
        d->table->resizeRowsToContents(); // costly on long lists
    }
    d->countLbl->setText(rows < d->visible.size()
                             ? QString("newest %1 of %2 shown").arg(rows).arg(d->visible.size())
                             : QString("%1 shown").arg(rows));
}

void ProblemsDialog::export_zip() {
    const QString suggested =
        QDir(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation))
            .filePath(QString("mosaic-problems-%1-%2.zip")
                          .arg(QSysInfo::machineHostName(),
                               QDateTime::currentDateTime().toString("yyyyMMdd-hhmm")));
    const QString zip =
        QFileDialog::getSaveFileName(this, "Export problems", suggested, "Zip archive (*.zip)");
    if (zip.isEmpty()) {
        return;
    }

    QTemporaryDir staging;
    if (!staging.isValid()) {
        d->statusLbl->setText("Could not create a temporary folder for the export.");
        return;
    }
    // The entries shown, as the journal stores them, plus a short summary.
    {
        QFile f(staging.filePath("problems.jsonl"));
        if (f.open(QIODevice::WriteOnly)) {
            for (auto it = d->visible.crbegin(); it != d->visible.crend(); ++it) {
                f.write(QJsonDocument(it->to_json()).toJson(QJsonDocument::Compact) + '\n');
            }
        }
        QFile s(staging.filePath("summary.txt"));
        if (s.open(QIODevice::WriteOnly | QIODevice::Text)) {
            s.write(QString("Computer: %1\nMOSAIC: %2\nWindows: %3\nExported: %4 by %5\n"
                            "Entries: %6\n")
                        .arg(QSysInfo::machineHostName(), QCoreApplication::applicationVersion(),
                             QSysInfo::prettyProductName(),
                             QDateTime::currentDateTime().toString(Qt::ISODate), d->username)
                        .arg(d->visible.size())
                        .toUtf8());
        }
    }
    for (const ExportFile& f :
         problem_export_files(d->root, d->username, d->isAdmin, d->dumpsBox->isChecked())) {
        const QString dest = staging.filePath(f.name);
        QDir().mkpath(QFileInfo(dest).absolutePath());
        QFile::copy(f.source, dest);
    }

    // Windows 10 and later ship bsdtar, which writes .zip with -a.
    QFile::remove(zip);
    QProcess tar;
    tar.start("tar", {"-a", "-c", "-f", zip, "-C", staging.path(), "."});
    const bool ok = tar.waitForFinished(120000) && tar.exitStatus() == QProcess::NormalExit &&
                    tar.exitCode() == 0 && QFileInfo(zip).size() > 0;
    if (ok) {
        d->statusLbl->setText("Exported to " + QDir::toNativeSeparators(zip));
        return;
    }
    // No zip tool: leave the files in a folder next to where the zip would go.
    const QString folder = QFileInfo(zip).absolutePath() + "/" + QFileInfo(zip).completeBaseName();
    QDirIterator it(staging.path(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString src  = it.next();
        const QString dest = folder + "/" + QDir(staging.path()).relativeFilePath(src);
        QDir().mkpath(QFileInfo(dest).absolutePath());
        QFile::copy(src, dest);
    }
    d->statusLbl->setText("Could not make a .zip; the files are in " +
                          QDir::toNativeSeparators(folder));
}

} // namespace mosaic
