#include "ui/session/session_archive_dialog.hpp"

#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStorageInfo>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <atomic>

#include "session/session_archive.hpp"
#include "utils/logger.hpp"

namespace mosaic {

namespace {

enum Column { ColSession = 0, ColAge, ColSize, ColNote, ColCount };

QString clean(const QString& path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }

// True when `inner` is `outer` or a folder inside it.
bool is_within(const QString& inner, const QString& outer) {
    const QString a = clean(inner);
    const QString b = clean(outer);
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    return a.compare(b, cs) == 0 || a.startsWith(b + "/", cs);
}

QString space_text(const QString& path) {
    QString existing = path;
    while (!existing.isEmpty() && !QFileInfo::exists(existing)) {
        const QString parent = QFileInfo(existing).absolutePath();
        if (parent == existing) {
            break;
        }
        existing = parent;
    }
    const QStorageInfo s(existing);
    if (!s.isValid()) {
        return "free space unknown";
    }
    return QString("%1 free of %2")
        .arg(format_bytes(s.bytesAvailable()), format_bytes(s.bytesTotal()));
}

} // namespace

struct SessionArchiveDialog::Impl {
    QList<SessionInfo> sessions;
    QList<qint64> sizes;
    QList<QString> blockers;
    QString recordsDir;
    RecordSettings* record = nullptr;
    QString activeSessionPath;
    QString username;

    QLineEdit* folderEdit      = nullptr;
    QSpinBox* daysSpin         = nullptr;
    QLabel* recordsSpace       = nullptr;
    QLabel* archiveSpace       = nullptr;
    QTreeWidget* tree          = nullptr;
    QLabel* selectionLbl       = nullptr;
    QPushButton* archiveBtn    = nullptr;
    QPushButton* closeBtn      = nullptr;
    QPushButton* cancelBtn     = nullptr;
    QProgressBar* progress     = nullptr;
    QLabel* progressLbl        = nullptr;
    QPlainTextEdit* resultsLog = nullptr;

    QThread* worker = nullptr;
    std::atomic<bool> cancel{false};
    bool movedAny   = false;
    int movedCount  = 0;
    int failedCount = 0;
};

SessionArchiveDialog::SessionArchiveDialog(const QList<SessionInfo>& sessions,
                                           const QString& recordsDir, RecordSettings* record,
                                           const QString& activeSessionPath,
                                           const QString& username, QWidget* parent)
    : QDialog(parent), d(std::make_unique<Impl>()) {
    d->username          = username;
    d->recordsDir        = recordsDir;
    d->record            = record;
    d->activeSessionPath = activeSessionPath;
    for (const auto& s : sessions) {
        if (s.archived) {
            continue;
        }
        d->sessions << s;
        d->sizes << directory_size(s.path);
        d->blockers << archive_blocker(s, activeSessionPath);
    }
    setWindowTitle("Archive sessions");
    resize(860, 620);
    build();
    apply_suggestions();
    refresh_space();
}

SessionArchiveDialog::~SessionArchiveDialog() {
    if (d->worker) {
        d->cancel = true;
        d->worker->wait();
        delete d->worker;
    }
}

bool SessionArchiveDialog::moved_any() const { return d->movedAny; }

void SessionArchiveDialog::build() {
    auto* box = new QVBoxLayout(this);
    box->setSpacing(10);

    auto* intro = new QLabel(
        "Move finished sessions to an archive folder to free the recordings drive. Each "
        "session is copied, every file is read back and compared, and only then is the "
        "original deleted. Archived sessions stay in the Session Browser, marked ARCHIVED, "
        "and can be moved back.");
    intro->setWordWrap(true);
    intro->setProperty("role", "muted");
    box->addWidget(intro);

    // Archive folder
    auto* folderRow = new QHBoxLayout;
    auto* folderLbl = new QLabel("Archive folder:");
    folderLbl->setFixedWidth(130);
    d->folderEdit = new QLineEdit(d->record ? d->record->archiveDirectory : QString());
    d->folderEdit->setReadOnly(true);
    d->folderEdit->setPlaceholderText("Not set: choose a folder on another drive or a share");
    auto* chooseBtn = new QPushButton("Choose…");
    connect(chooseBtn, &QPushButton::clicked, this, &SessionArchiveDialog::choose_folder);
    folderRow->addWidget(folderLbl);
    folderRow->addWidget(d->folderEdit, 1);
    folderRow->addWidget(chooseBtn);
    box->addLayout(folderRow);

    // Suggestion age
    auto* daysRow = new QHBoxLayout;
    auto* daysLbl = new QLabel("Suggest sessions older than");
    d->daysSpin   = new QSpinBox;
    d->daysSpin->setRange(1, RecordSettings::kMaxArchiveAfterDays);
    d->daysSpin->setValue(d->record ? d->record->archiveAfterDays : 30);
    d->daysSpin->setSuffix(" days");
    auto* applyBtn = new QPushButton("Tick them");
    applyBtn->setToolTip("Tick every session older than this, and untick the rest.");
    connect(d->daysSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
        if (d->record) {
            d->record->archiveAfterDays = v;
        }
    });
    connect(applyBtn, &QPushButton::clicked, this, &SessionArchiveDialog::apply_suggestions);
    daysRow->addWidget(daysLbl);
    daysRow->addWidget(d->daysSpin);
    daysRow->addWidget(applyBtn);
    daysRow->addStretch();
    box->addLayout(daysRow);

    // Disk space
    d->recordsSpace = new QLabel;
    d->archiveSpace = new QLabel;
    box->addWidget(d->recordsSpace);
    box->addWidget(d->archiveSpace);

    // Sessions
    d->tree = new QTreeWidget;
    d->tree->setColumnCount(ColCount);
    d->tree->setHeaderLabels({"Session", "Age", "Size", ""});
    d->tree->setRootIsDecorated(false);
    d->tree->setUniformRowHeights(true);
    d->tree->header()->setSectionResizeMode(ColSession, QHeaderView::Stretch);
    d->tree->header()->setSectionResizeMode(ColAge, QHeaderView::ResizeToContents);
    d->tree->header()->setSectionResizeMode(ColSize, QHeaderView::ResizeToContents);
    d->tree->header()->setSectionResizeMode(ColNote, QHeaderView::ResizeToContents);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (int i = 0; i < d->sessions.size(); ++i) {
        const auto& s = d->sessions[i];
        auto* item    = new QTreeWidgetItem;
        const QString when =
            s.startUtc.isValid() ? s.startUtc.toLocalTime().toString("yyyy-MM-dd hh:mm") : "?";
        item->setText(ColSession, when + "   " + s.name);
        item->setToolTip(ColSession, QDir::toNativeSeparators(s.path));
        item->setText(ColAge, s.startUtc.isValid() ? QString("%1 d").arg(s.startUtc.daysTo(now))
                                                   : QString("?"));
        item->setText(ColSize, format_bytes(d->sizes[i]));
        item->setTextAlignment(ColAge, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(ColSize, Qt::AlignRight | Qt::AlignVCenter);
        if (!d->blockers[i].isEmpty()) {
            item->setText(ColNote, d->blockers[i]);
            item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable & ~Qt::ItemIsEnabled);
        } else {
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(ColSession, Qt::Unchecked);
            if (s.interrupted) {
                item->setText(ColNote, "interrupted recording");
            }
        }
        item->setData(ColSession, Qt::UserRole, i);
        d->tree->addTopLevelItem(item);
    }
    connect(d->tree, &QTreeWidget::itemChanged, this, &SessionArchiveDialog::refresh_selection);
    box->addWidget(d->tree, 1);

    // Progress and results
    d->progressLbl = new QLabel;
    d->progressLbl->setProperty("role", "muted");
    d->progress = new QProgressBar;
    d->progress->setRange(0, 1000);
    d->resultsLog = new QPlainTextEdit;
    d->resultsLog->setReadOnly(true);
    d->resultsLog->setMaximumHeight(110);
    d->progressLbl->hide();
    d->progress->hide();
    d->resultsLog->hide();
    box->addWidget(d->progressLbl);
    box->addWidget(d->progress);
    box->addWidget(d->resultsLog);

    // Buttons
    auto* footer    = new QHBoxLayout;
    d->selectionLbl = new QLabel;
    d->archiveBtn   = new QPushButton("Archive");
    d->archiveBtn->setDefault(true);
    d->cancelBtn = new QPushButton("Cancel");
    d->cancelBtn->hide();
    d->closeBtn = new QPushButton("Close");
    connect(d->archiveBtn, &QPushButton::clicked, this, &SessionArchiveDialog::start);
    connect(d->cancelBtn, &QPushButton::clicked, this, [this] {
        d->cancel = true;
        d->cancelBtn->setEnabled(false);
        d->progressLbl->setText("Cancelling: the session being copied stays where it is…");
    });
    connect(d->closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    footer->addWidget(d->selectionLbl, 1);
    footer->addWidget(d->archiveBtn);
    footer->addWidget(d->cancelBtn);
    footer->addWidget(d->closeBtn);
    box->addLayout(footer);
}

void SessionArchiveDialog::apply_suggestions() {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const int days      = d->daysSpin->value();
    d->tree->blockSignals(true);
    for (int r = 0; r < d->tree->topLevelItemCount(); ++r) {
        auto* item = d->tree->topLevelItem(r);
        if (!(item->flags() & Qt::ItemIsUserCheckable)) {
            continue;
        }
        const int i = item->data(ColSession, Qt::UserRole).toInt();
        item->setCheckState(
            ColSession, suggest_archiving(d->sessions[i], days, now) ? Qt::Checked : Qt::Unchecked);
    }
    d->tree->blockSignals(false);
    refresh_selection();
}

void SessionArchiveDialog::refresh_space() {
    qint64 total = 0;
    for (qint64 s : d->sizes) {
        total += s;
    }
    d->recordsSpace->setText(QString("Recordings drive: %1 · these sessions use %2")
                                 .arg(space_text(d->recordsDir), format_bytes(total)));
    const QString archive = d->record ? d->record->archiveDirectory : QString();
    d->archiveSpace->setText(archive.isEmpty() ? "Archive drive: no archive folder chosen yet"
                                               : "Archive drive: " + space_text(archive));
    refresh_selection();
}

void SessionArchiveDialog::refresh_selection() {
    int n        = 0;
    qint64 bytes = 0;
    for (int r = 0; r < d->tree->topLevelItemCount(); ++r) {
        auto* item = d->tree->topLevelItem(r);
        if (item->checkState(ColSession) == Qt::Checked) {
            ++n;
            bytes += d->sizes[item->data(ColSession, Qt::UserRole).toInt()];
        }
    }
    const bool haveFolder = d->record && !d->record->archiveDirectory.isEmpty();
    d->selectionLbl->setText(n == 0 ? QString("No session ticked")
                                    : QString("%1 session%2 ticked, %3")
                                          .arg(n)
                                          .arg(n == 1 ? "" : "s")
                                          .arg(format_bytes(bytes)));
    d->archiveBtn->setText(n > 0 ? QString("Archive %1").arg(n) : QString("Archive"));
    d->archiveBtn->setEnabled(n > 0 && haveFolder && !d->worker);
    d->archiveBtn->setToolTip(haveFolder ? QString() : "Choose an archive folder first.");
}

void SessionArchiveDialog::choose_folder() {
    const QString start = d->record && !d->record->archiveDirectory.isEmpty()
                              ? d->record->archiveDirectory
                              : QString();
    const QString dir = QFileDialog::getExistingDirectory(this, "Choose the archive folder", start);
    if (dir.isEmpty() || !d->record) {
        return;
    }
    // Inside the recordings folder (or the other way round), archived
    // sessions would be listed and moved twice.
    if (is_within(dir, d->recordsDir) || is_within(d->recordsDir, dir)) {
        d->archiveSpace->setText(
            "That folder is inside the recordings folder (or contains it). Choose a folder "
            "elsewhere, ideally on another drive.");
        return;
    }
    d->record->archiveDirectory = QDir::cleanPath(dir);
    d->folderEdit->setText(QDir::toNativeSeparators(d->record->archiveDirectory));
    refresh_space();
}

void SessionArchiveDialog::start() {
    if (d->worker || !d->record || d->record->archiveDirectory.isEmpty()) {
        return;
    }
    QList<int> rows;
    for (int r = 0; r < d->tree->topLevelItemCount(); ++r) {
        if (d->tree->topLevelItem(r)->checkState(ColSession) == Qt::Checked) {
            rows << r;
        }
    }
    if (rows.isEmpty()) {
        return;
    }
    qint64 total = 0;
    struct Job {
        int row;
        QString path;
        qint64 size;
    };
    QList<Job> jobs;
    for (int r : rows) {
        const int i = d->tree->topLevelItem(r)->data(ColSession, Qt::UserRole).toInt();
        total += d->sizes[i];
        jobs.append({r, d->sessions[i].path, d->sizes[i]});
    }

    d->cancel      = false;
    d->movedCount  = 0;
    d->failedCount = 0;
    d->tree->setEnabled(false);
    d->daysSpin->setEnabled(false);
    d->archiveBtn->setEnabled(false);
    d->closeBtn->setEnabled(false);
    d->cancelBtn->setEnabled(true);
    d->cancelBtn->show();
    d->progress->setValue(0);
    d->progress->show();
    d->progressLbl->show();
    d->resultsLog->clear();
    d->resultsLog->show();

    const QString dest = d->record->archiveDirectory;
    log_info(QString("[Archive] Moving %1 session(s), %2, to %3")
                 .arg(jobs.size())
                 .arg(format_bytes(total))
                 .arg(dest));

    const QString archivedBy = d->username;
    d->worker                = QThread::create([this, jobs, dest, total, archivedBy] {
        qint64 before = 0;
        for (const auto& [row, path, size] : jobs) {
            if (d->cancel) {
                QMetaObject::invokeMethod(this, [this, row = row] {
                    finish_one(row, static_cast<int>(ArchiveResult::Status::Cancelled),
                                              "Not moved (cancelled).");
                });
                continue;
            }
            const QString name = QFileInfo(path).fileName();
            ArchiveOptions opt;
            opt.archivedBy = archivedBy;
            opt.cancel     = &d->cancel;
            opt.progress   = [this, before, total, name](const ArchiveProgress& p) {
                // Copy and read-back each count half of a session's share.
                const double share =
                    total > 0
                                         ? (before + (p.verifying ? p.bytesTotal : 0) / 2.0 + p.bytesDone / 2.0) /
                              static_cast<double>(total)
                                         : 1.0;
                const QString what =
                    QString("%1 %2: %3").arg(p.verifying ? "Checking" : "Copying", name, p.file);
                QMetaObject::invokeMethod(this, [this, share, what] {
                    d->progress->setValue(static_cast<int>(1000.0 * std::min(1.0, share)));
                    d->progressLbl->setText(what);
                });
            };
            const ArchiveResult r = move_session_verified(path, dest, opt);
            before += size;
            QMetaObject::invokeMethod(this, [this, row = row, r] {
                finish_one(row, static_cast<int>(r.status), r.message);
            });
        }
        QMetaObject::invokeMethod(this, [this] { finish_all(); });
    });
    d->worker->start();
}

void SessionArchiveDialog::finish_one(int row, int status, const QString& message) {
    auto* item    = d->tree->topLevelItem(row);
    const auto st = static_cast<ArchiveResult::Status>(status);
    if (st == ArchiveResult::Status::Moved) {
        ++d->movedCount;
        d->movedAny = true;
        item->setText(ColNote, "archived");
        item->setCheckState(ColSession, Qt::Unchecked);
        item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable & ~Qt::ItemIsEnabled);
        log_info("[Archive] " + message);
    } else if (st == ArchiveResult::Status::Failed) {
        ++d->failedCount;
        item->setText(ColNote, "not moved");
        log_warning("[Archive] " + message);
    }
    d->resultsLog->appendPlainText(message);
}

void SessionArchiveDialog::finish_all() {
    if (d->worker) {
        d->worker->wait();
        d->worker->deleteLater();
        d->worker = nullptr;
    }
    d->progress->setValue(1000);
    d->progressLbl->setText(QString("Done: %1 moved%2.")
                                .arg(d->movedCount)
                                .arg(d->failedCount
                                         ? QString(", %1 not moved (see below)").arg(d->failedCount)
                                         : QString()));
    d->cancelBtn->hide();
    d->closeBtn->setEnabled(true);
    d->tree->setEnabled(true);
    d->daysSpin->setEnabled(true);
    // Sizes changed on both drives.
    refresh_space();
}

void SessionArchiveDialog::reject() {
    if (d->worker) {
        return; // Cancel first; the dialog closes once nothing is in flight.
    }
    QDialog::reject();
}

} // namespace mosaic
