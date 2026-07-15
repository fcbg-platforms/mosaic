#include "ui/auth/admin_panel_dialog.hpp"
#include <QCheckBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace mosaic {

// ── Compact profile row widget ─────────────────────────────────────────────

class ProfileRow : public QWidget {
    Q_OBJECT
public:
    explicit ProfileRow(const Profile& prof, QWidget* parent = nullptr)
        : QWidget(parent), m_username(prof.username)
    {
        setFixedHeight(58);
        setCursor(Qt::PointingHandCursor);

        auto* lay  = new QHBoxLayout(this);
        lay->setContentsMargins(10, 6, 12, 6);
        lay->setSpacing(10);

        // Mini avatar circle
        m_avatar = new QLabel;
        m_avatar->setFixedSize(40, 40);
        m_avatar->setAlignment(Qt::AlignCenter);
        set_avatar(prof);
        lay->addWidget(m_avatar);

        // Name column
        auto* nameCol = new QVBoxLayout;
        nameCol->setSpacing(1);

        m_nameLbl = new QLabel(prof.displayName.isEmpty() ? prof.username : prof.displayName);
        m_nameLbl->setStyleSheet("color: #c8c8e0; font-size: 12px; font-weight: bold;");
        nameCol->addWidget(m_nameLbl);

        QString sub = "@" + prof.username;
        if (!prof.institution.isEmpty()) sub += "  ·  " + prof.institution;
        m_subLbl = new QLabel(sub);
        m_subLbl->setStyleSheet("color: #555577; font-size: 10px;");
        nameCol->addWidget(m_subLbl);

        lay->addLayout(nameCol, 1);

        // Role badge
        if (prof.is_admin()) {
            auto* badge = new QLabel("♚ Admin");
            badge->setStyleSheet("color: #ffcc00; background: #1a1400; border: 1px solid #664400;"
                                 " border-radius: 8px; padding: 2px 7px; font-size: 10px;");
            lay->addWidget(badge);
        }

        // Last-login label
        QString lastSeen;
        if (prof.lastLogin.isValid()) {
            const qint64 daysDiff = prof.lastLogin.daysTo(QDateTime::currentDateTimeUtc());
            if (daysDiff == 0) lastSeen = "today";
            else if (daysDiff == 1) lastSeen = "yesterday";
            else lastSeen = QString("%1d ago").arg(daysDiff);
        }
        if (!lastSeen.isEmpty()) {
            auto* lastLbl = new QLabel(lastSeen);
            lastLbl->setStyleSheet("color: #333355; font-size: 10px;");
            lay->addWidget(lastLbl);
        }
    }

    [[nodiscard]] QString username() const { return m_username; }

    void set_selected(bool selected) {
        if (m_selected == selected) return;
        m_selected = selected;
        setStyleSheet(selected
            ? "ProfileRow { background: #14143c; border-left: 3px solid #6666ee; }"
            : "ProfileRow { background: transparent; border-left: 3px solid transparent; }");
    }

signals:
    void clicked(QString username);

protected:
    void mousePressEvent(QMouseEvent*) override { emit clicked(m_username); }
    void enterEvent(QEnterEvent*)      override { if (!m_selected) setStyleSheet(
        "ProfileRow { background: #0e0e28; border-left: 3px solid transparent; }"); }
    void leaveEvent(QEvent*)           override { if (!m_selected) setStyleSheet(
        "ProfileRow { background: transparent; border-left: 3px solid transparent; }"); }

private:
    void set_avatar(const Profile& prof) {
        const QColor accent(prof.accentColour.isEmpty() ? "#5566bb" : prof.accentColour);
        QPixmap pix(40, 40);
        pix.fill(Qt::transparent);
        QPainter p(&pix);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(accent);
        p.setPen(Qt::NoPen);
        p.drawEllipse(0, 0, 40, 40);
        QFont font;
        font.setPointSize(12);
        font.setBold(true);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(QRectF(0, 0, 40, 40), Qt::AlignCenter,
                   prof.initials.isEmpty() ? "?" : prof.initials);
        m_avatar->setPixmap(pix);
    }

    QString m_username;
    QLabel* m_avatar  = nullptr;
    QLabel* m_nameLbl = nullptr;
    QLabel* m_subLbl  = nullptr;
    bool    m_selected{false};
};

// ── Impl ───────────────────────────────────────────────────────────────────

struct AdminPanelDialog::Impl {
    ProfileManager& profileMgr;
    QString         adminUsername;
    QString         selectedUsername;   // chosen profile to launch as
    QString         detailUsername;     // profile whose detail pane is showing

    // Left panel
    QVBoxLayout*       listLayout  = nullptr;
    QList<ProfileRow*> rows;

    // Right panel / detail
    QLabel*      detailHeader    = nullptr;
    QLineEdit*   nameEdit        = nullptr;
    QLineEdit*   institutionEdit = nullptr;
    QLabel*      roleLabel       = nullptr;
    QPushButton* roleBtn         = nullptr;
    QPushButton* launchBtn       = nullptr;

    explicit Impl(ProfileManager& mgr, const QString& admin)
        : profileMgr(mgr), adminUsername(admin), selectedUsername(admin) {}
};

// ── AdminPanelDialog ────────────────────────────────────────────────────────

AdminPanelDialog::AdminPanelDialog(ProfileManager& profileMgr,
                                    const QString&  adminUsername,
                                    QWidget*        parent)
    : QDialog(parent, Qt::Window | Qt::FramelessWindowHint)
    , d(std::make_unique<Impl>(profileMgr, adminUsername))
{
    setModal(true);
    setMinimumSize(860, 580);
    resize(960, 640);
    setStyleSheet(R"(
        AdminPanelDialog, QDialog { background: #08081a; }
        QLineEdit {
            background: #0a0a20; border: 1px solid #252545;
            border-radius: 4px; padding: 6px 10px;
            color: #c8c8e0; font-size: 12px;
        }
        QLineEdit:focus { border-color: #6060ee; background: #0d0d28; }
        QPushButton {
            background: #12122e; border: 1px solid #252545;
            border-radius: 4px; padding: 6px 14px;
            color: #9898cc; font-size: 11px;
        }
        QPushButton:hover { background: #1a1a40; border-color: #4444aa; color: #bbbbff; }
        QPushButton:pressed { background: #0d0d22; }
        QLabel[role="section"] {
            color: #44446a; font-size: 9px; font-weight: bold; letter-spacing: 2px;
        }
    )");

    build_ui();

    connect(&profileMgr, &ProfileManager::profiles_changed,
            this, &AdminPanelDialog::refresh_profile_list);
}

AdminPanelDialog::~AdminPanelDialog() = default;

QString AdminPanelDialog::selected_username() const { return d->selectedUsername; }

// ── UI construction ────────────────────────────────────────────────────────

void AdminPanelDialog::build_ui() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Header bar ──────────────────────────────────────────────────────────
    auto* headerBar = new QWidget;
    headerBar->setFixedHeight(52);
    headerBar->setStyleSheet("background: #0c0c22; border-bottom: 1px solid #1a1a35;");
    auto* headerLay = new QHBoxLayout(headerBar);
    headerLay->setContentsMargins(20, 0, 16, 0);

    auto* logo = new QLabel("◈  MOSAIC  —  Admin Panel");
    logo->setStyleSheet("color: #ffcc44; font-size: 16px; font-weight: bold; letter-spacing: 2px;");
    headerLay->addWidget(logo);
    headerLay->addStretch();

    auto* adminBadge = new QLabel(QString("Logged in as  @%1").arg(d->adminUsername));
    adminBadge->setStyleSheet("color: #555577; font-size: 11px;");
    headerLay->addWidget(adminBadge);
    headerLay->addSpacing(16);

    auto* backBtn = new QPushButton("← Back to Login");
    backBtn->setFixedHeight(30);
    backBtn->setStyleSheet("QPushButton { background: transparent; border: 1px solid #252545;"
                           " border-radius: 4px; padding: 4px 12px; color: #666688; font-size: 11px; }"
                           "QPushButton:hover { color: #9999cc; border-color: #44446a; }");
    connect(backBtn, &QPushButton::clicked, this, &QDialog::reject);
    headerLay->addWidget(backBtn);

    root->addWidget(headerBar);

    // ── Main splitter: list | detail ────────────────────────────────────────
    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->setHandleWidth(1);
    splitter->setStyleSheet("QSplitter::handle { background: #1a1a35; }");

    // Left: profile list
    auto* listPanel = new QWidget;
    listPanel->setMinimumWidth(240);
    listPanel->setMaximumWidth(320);
    listPanel->setStyleSheet("background: #090918;");
    build_profile_list(listPanel);
    splitter->addWidget(listPanel);

    // Right: detail
    auto* detailPanel = new QWidget;
    detailPanel->setStyleSheet("background: #08081a;");
    build_detail_pane(detailPanel);
    splitter->addWidget(detailPanel);

    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({270, 690});

    root->addWidget(splitter, 1);

    // ── Footer ───────────────────────────────────────────────────────────────
    auto* footer = new QWidget;
    footer->setFixedHeight(56);
    footer->setStyleSheet("background: #0c0c22; border-top: 1px solid #1a1a35;");
    auto* footerLay = new QHBoxLayout(footer);
    footerLay->setContentsMargins(20, 0, 20, 0);
    footerLay->setSpacing(10);

    auto* hintLbl = new QLabel("Select a profile on the left, then choose how to enter MOSAIC.");
    hintLbl->setStyleSheet("color: #333355; font-size: 11px;");
    footerLay->addWidget(hintLbl, 1);

    d->launchBtn = new QPushButton("▶  Launch as selected profile");
    d->launchBtn->setEnabled(false);
    d->launchBtn->setFixedHeight(34);
    d->launchBtn->setStyleSheet(
        "QPushButton { background: #1a3a1a; border: 1px solid #33aa55; border-radius: 5px;"
        "  padding: 6px 18px; color: #88ffaa; font-size: 12px; font-weight: bold; }"
        "QPushButton:hover { background: #224a22; border-color: #44cc66; }"
        "QPushButton:pressed { background: #103010; }"
        "QPushButton:disabled { background: #0d0d22; border-color: #1e1e3a; color: #333355; }");
    connect(d->launchBtn, &QPushButton::clicked, this, [this] {
        if (!d->detailUsername.isEmpty()) {
            d->selectedUsername = d->detailUsername;
        }
        accept();
    });
    footerLay->addWidget(d->launchBtn);

    auto* enterAsAdminBtn = new QPushButton("⚙  Enter as Admin");
    enterAsAdminBtn->setFixedHeight(34);
    enterAsAdminBtn->setStyleSheet(
        "QPushButton { background: #1a1a3a; border: 1px solid #4444cc; border-radius: 5px;"
        "  padding: 6px 18px; color: #aaaaff; font-size: 12px; font-weight: bold; }"
        "QPushButton:hover { background: #22226a; border-color: #6666ee; }"
        "QPushButton:pressed { background: #101030; }");
    connect(enterAsAdminBtn, &QPushButton::clicked, this, [this] {
        d->selectedUsername = d->adminUsername;
        accept();
    });
    footerLay->addWidget(enterAsAdminBtn);

    root->addWidget(footer);
}

// ── Left panel: profile list ───────────────────────────────────────────────

void AdminPanelDialog::build_profile_list(QWidget* parent) {
    auto* outer = new QVBoxLayout(parent);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Section header
    auto* hdr = new QWidget;
    hdr->setFixedHeight(36);
    hdr->setStyleSheet("background: #0c0c20; border-bottom: 1px solid #1a1a32;");
    auto* hdrLay = new QHBoxLayout(hdr);
    hdrLay->setContentsMargins(12, 0, 12, 0);
    auto* hdrLbl = new QLabel("RESEARCH GROUPS");
    hdrLbl->setProperty("role", "section");
    hdrLay->addWidget(hdrLbl);
    outer->addWidget(hdr);

    // Scrollable list
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea { background: transparent; }");

    auto* listW = new QWidget;
    listW->setStyleSheet("background: transparent;");
    d->listLayout = new QVBoxLayout(listW);
    d->listLayout->setContentsMargins(0, 4, 0, 4);
    d->listLayout->setSpacing(0);
    d->listLayout->addStretch();

    scroll->setWidget(listW);
    outer->addWidget(scroll, 1);

    refresh_profile_list();
}

void AdminPanelDialog::refresh_profile_list() {
    // Remove existing rows
    for (auto* row : d->rows) { row->deleteLater(); }
    d->rows.clear();

    // Remove all items except the trailing stretch
    while (d->listLayout->count() > 1) {
        auto* item = d->listLayout->takeAt(0);
        delete item;
    }

    const auto profiles = d->profileMgr.profiles();
    for (const auto& prof : profiles) {
        auto* row = new ProfileRow(prof, nullptr);
        connect(row, &ProfileRow::clicked, this, &AdminPanelDialog::on_profile_selected);
        d->listLayout->insertWidget(d->listLayout->count() - 1, row);
        d->rows.append(row);

        if (prof.username == d->detailUsername) {
            row->set_selected(true);
        }
    }
}

// ── Right panel: detail ────────────────────────────────────────────────────

void AdminPanelDialog::build_detail_pane(QWidget* parent) {
    auto* lay = new QVBoxLayout(parent);
    lay->setContentsMargins(28, 24, 28, 24);
    lay->setSpacing(0);

    // Placeholder shown when no profile is selected
    d->detailHeader = new QLabel("Select a profile on the left to manage it.");
    d->detailHeader->setStyleSheet("color: #333355; font-size: 14px;");
    d->detailHeader->setAlignment(Qt::AlignCenter);
    lay->addWidget(d->detailHeader);

    lay->addStretch();

    // Profile editing form (initially hidden)
    auto make_section = [&](const QString& title) {
        lay->addSpacing(14);
        auto* lbl = new QLabel(title);
        lbl->setProperty("role", "section");
        lay->addWidget(lbl);
        lay->addSpacing(6);
    };

    // Identity section
    make_section("IDENTITY");

    auto* nameRow = new QHBoxLayout;
    auto* nameLbl = new QLabel("Display name:");
    nameLbl->setStyleSheet("color: #7070a0; font-size: 11px;");
    nameLbl->setFixedWidth(115);
    nameLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    d->nameEdit = new QLineEdit;
    d->nameEdit->setPlaceholderText("e.g. Cognitive Science Lab");
    auto* nameSave = new QPushButton("Save");
    nameSave->setFixedWidth(56);
    connect(nameSave, &QPushButton::clicked, this, &AdminPanelDialog::save_display_name);
    nameRow->addWidget(nameLbl);
    nameRow->addSpacing(8);
    nameRow->addWidget(d->nameEdit, 1);
    nameRow->addSpacing(6);
    nameRow->addWidget(nameSave);
    lay->addLayout(nameRow);
    lay->addSpacing(8);

    auto* instRow = new QHBoxLayout;
    auto* instLbl = new QLabel("Institution:");
    instLbl->setStyleSheet("color: #7070a0; font-size: 11px;");
    instLbl->setFixedWidth(115);
    instLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    d->institutionEdit = new QLineEdit;
    d->institutionEdit->setPlaceholderText("e.g. MIT Brain & Cognitive Sciences");
    auto* instSave = new QPushButton("Save");
    instSave->setFixedWidth(56);
    connect(instSave, &QPushButton::clicked, this, &AdminPanelDialog::save_institution);
    instRow->addWidget(instLbl);
    instRow->addSpacing(8);
    instRow->addWidget(d->institutionEdit, 1);
    instRow->addSpacing(6);
    instRow->addWidget(instSave);
    lay->addLayout(instRow);

    // Role section
    make_section("ROLE");
    auto* roleRow = new QHBoxLayout;
    d->roleLabel = new QLabel;
    d->roleLabel->setStyleSheet("color: #9090c0; font-size: 12px;");
    d->roleBtn = new QPushButton;
    connect(d->roleBtn, &QPushButton::clicked, this, &AdminPanelDialog::toggle_role);
    roleRow->addWidget(d->roleLabel, 1);
    roleRow->addWidget(d->roleBtn);
    lay->addLayout(roleRow);

    // Security section
    make_section("SECURITY");
    auto* pwRow = new QHBoxLayout;
    auto* pwHint = new QLabel("Reset password for this profile");
    pwHint->setStyleSheet("color: #7070a0; font-size: 11px;");
    auto* pwBtn = new QPushButton("Reset password…");
    connect(pwBtn, &QPushButton::clicked, this, &AdminPanelDialog::reset_password);
    pwRow->addWidget(pwHint, 1);
    pwRow->addWidget(pwBtn);
    lay->addLayout(pwRow);

    // Config section
    make_section("CONFIGURATION");
    auto* cfgRow = new QHBoxLayout;
    cfgRow->setSpacing(8);
    auto* folderBtn = new QPushButton("📂  Open folder");
    connect(folderBtn, &QPushButton::clicked, this, &AdminPanelDialog::open_config_folder);
    auto* exportBtn = new QPushButton("⬆  Export config…");
    connect(exportBtn, &QPushButton::clicked, this, &AdminPanelDialog::export_config);
    auto* importBtn = new QPushButton("⬇  Import config…");
    connect(importBtn, &QPushButton::clicked, this, &AdminPanelDialog::import_config);
    cfgRow->addWidget(folderBtn);
    cfgRow->addWidget(exportBtn);
    cfgRow->addWidget(importBtn);
    cfgRow->addStretch();
    lay->addLayout(cfgRow);

    // Danger zone
    make_section("DANGER ZONE");
    auto* dangerRow = new QHBoxLayout;
    auto* dangerHint = new QLabel("Permanently removes the profile from the manifest (data stays on disk).");
    dangerHint->setWordWrap(true);
    dangerHint->setStyleSheet("color: #554444; font-size: 11px;");
    auto* deleteBtn = new QPushButton("🗑  Delete profile…");
    deleteBtn->setStyleSheet(
        "QPushButton { background: #2a0808; border: 1px solid #661111; border-radius: 4px;"
        "  padding: 6px 14px; color: #cc5555; font-size: 11px; }"
        "QPushButton:hover { background: #3a1010; border-color: #aa2222; color: #ff7777; }"
        "QPushButton:pressed { background: #1a0404; }");
    connect(deleteBtn, &QPushButton::clicked, this, &AdminPanelDialog::delete_profile);
    dangerRow->addWidget(dangerHint, 1);
    dangerRow->addWidget(deleteBtn);
    lay->addLayout(dangerRow);

    lay->addStretch();

    // Hide editing sections until a profile is chosen
    const bool show = !d->detailUsername.isEmpty();
    nameRow->setEnabled(show);
    instRow->setEnabled(show);
    roleRow->setEnabled(show);
    pwRow->setEnabled(show);
    cfgRow->setEnabled(show);
    dangerRow->setEnabled(show);
}

// ── Profile selection ──────────────────────────────────────────────────────

void AdminPanelDialog::on_profile_selected(const QString& username) {
    d->detailUsername = username;

    for (auto* row : d->rows) {
        row->set_selected(row->username() == username);
    }

    populate_detail(username);

    d->launchBtn->setEnabled(true);
    d->launchBtn->setText(QString("▶  Launch as  @%1").arg(username));
}

void AdminPanelDialog::populate_detail(const QString& username) {
    const Profile* prof = d->profileMgr.find(username);
    if (!prof) return;

    d->detailHeader->hide();

    d->nameEdit->setText(prof->displayName);
    d->institutionEdit->setText(prof->institution);

    if (prof->is_admin()) {
        d->roleLabel->setText("This profile has  <b style='color:#ffcc44'>Admin</b>  privileges.");
        d->roleBtn->setText("Demote to User");
        d->roleBtn->setStyleSheet("QPushButton { background: #1a1000; border: 1px solid #664400;"
                                  " color: #cc8800; } "
                                  "QPushButton:hover { background: #2a1800; border-color: #aa6600; color: #ffaa00; }");
    } else {
        d->roleLabel->setText("This profile has standard  <b style='color:#8888cc'>User</b>  privileges.");
        d->roleBtn->setText("Promote to Admin");
        d->roleBtn->setStyleSheet("QPushButton { background: #0a0a1a; border: 1px solid #333388;"
                                  " color: #7777cc; } "
                                  "QPushButton:hover { background: #14143a; border-color: #5555cc; color: #aaaaff; }");
    }

    // Prevent deleting the currently logged-in admin account
    d->roleBtn->setEnabled(username != d->adminUsername);
}

// ── Actions ────────────────────────────────────────────────────────────────

void AdminPanelDialog::save_display_name() {
    if (d->detailUsername.isEmpty()) return;
    const QString newName = d->nameEdit->text().trimmed();
    if (!newName.isEmpty()) {
        d->profileMgr.rename_display(d->detailUsername, newName);
    }
}

void AdminPanelDialog::save_institution() {
    if (d->detailUsername.isEmpty()) return;
    d->profileMgr.set_institution(d->detailUsername, d->institutionEdit->text().trimmed());
}

void AdminPanelDialog::reset_password() {
    if (d->detailUsername.isEmpty()) return;

    bool ok = false;
    const QString newPw = QInputDialog::getText(
        this, "Reset password",
        QString("New password for @%1  (leave blank to remove password):").arg(d->detailUsername),
        QLineEdit::Password, {}, &ok);

    if (!ok) return;

    if (!newPw.isEmpty() && newPw.length() < 4) {
        QMessageBox::warning(this, "Too short", "Password must be at least 4 characters.");
        return;
    }

    d->profileMgr.change_password(d->detailUsername, newPw);

    const QString msg = newPw.isEmpty()
        ? QString("Password removed from @%1.").arg(d->detailUsername)
        : QString("Password updated for @%1.").arg(d->detailUsername);
    QMessageBox::information(this, "Done", msg);
}

void AdminPanelDialog::toggle_role() {
    if (d->detailUsername.isEmpty() || d->detailUsername == d->adminUsername) return;

    const Profile* prof = d->profileMgr.find(d->detailUsername);
    if (!prof) return;

    const bool isAdmin = prof->is_admin();
    const QString action = isAdmin ? "demote" : "promote";
    const auto btn = QMessageBox::question(
        this, "Change role",
        QString("Are you sure you want to %1 @%2 %3?")
            .arg(action, d->detailUsername,
                 isAdmin ? "to a regular user" : "to admin"));

    if (btn != QMessageBox::Yes) return;

    d->profileMgr.set_role(d->detailUsername,
        isAdmin ? Profile::Role::User : Profile::Role::Admin);
    populate_detail(d->detailUsername);
}

void AdminPanelDialog::open_config_folder() {
    if (d->detailUsername.isEmpty()) return;
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(ProfileManager::profile_dir(d->detailUsername)));
}

void AdminPanelDialog::export_config() {
    if (d->detailUsername.isEmpty()) return;

    const QString src = ProfileManager::settings_path(d->detailUsername);
    if (!QFile::exists(src)) {
        QMessageBox::information(this, "No config",
            "This profile has no saved configuration yet. "
            "Launch MOSAIC as this profile and save settings first.");
        return;
    }

    const QString dst = QFileDialog::getSaveFileName(
        this, "Export configuration",
        QString("%1_settings.json").arg(d->detailUsername),
        "JSON (*.json)");
    if (dst.isEmpty()) return;

    QFile::remove(dst);
    if (!QFile::copy(src, dst)) {
        QMessageBox::critical(this, "Export failed",
            "Could not copy the settings file. Check file permissions.");
    } else {
        QMessageBox::information(this, "Exported",
            QString("Configuration exported to:\n%1").arg(dst));
    }
}

void AdminPanelDialog::import_config() {
    if (d->detailUsername.isEmpty()) return;

    const QString src = QFileDialog::getOpenFileName(
        this, "Import configuration", {}, "JSON (*.json)");
    if (src.isEmpty()) return;

    const auto btn = QMessageBox::question(
        this, "Overwrite config",
        QString("This will replace the current settings for @%1.\nAre you sure?")
            .arg(d->detailUsername));
    if (btn != QMessageBox::Yes) return;

    const QString dst = ProfileManager::settings_path(d->detailUsername);
    QDir().mkpath(ProfileManager::profile_dir(d->detailUsername));
    QFile::remove(dst);
    if (!QFile::copy(src, dst)) {
        QMessageBox::critical(this, "Import failed",
            "Could not write the settings file. Check file permissions.");
    } else {
        QMessageBox::information(this, "Imported",
            QString("Configuration imported for @%1.").arg(d->detailUsername));
    }
}

void AdminPanelDialog::delete_profile() {
    if (d->detailUsername.isEmpty()) return;
    if (d->detailUsername == d->adminUsername) {
        QMessageBox::warning(this, "Cannot delete",
            "You cannot delete the currently logged-in admin account.");
        return;
    }

    const auto btn = QMessageBox::question(
        this, "Delete profile",
        QString("Are you sure you want to delete @%1?\n\n"
                "The profile data directory will remain on disk "
                "and can be recovered manually.")
            .arg(d->detailUsername),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);

    if (btn != QMessageBox::Yes) return;

    d->profileMgr.delete_profile(d->detailUsername);
    d->detailUsername.clear();
    d->launchBtn->setEnabled(false);
    d->launchBtn->setText("▶  Launch as selected profile");
    d->detailHeader->show();
}

// ── Key handling ───────────────────────────────────────────────────────────

void AdminPanelDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) { return; }
    QDialog::keyPressEvent(event);
}

} // namespace mosaic

#include "admin_panel_dialog.moc"
