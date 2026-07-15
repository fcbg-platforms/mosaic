#include "ui/auth/login_dialog.hpp"
#include <QCheckBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QPushButton>

namespace mosaic {

// ─────────────────────────────────────────────────────────────────────────────
// Branding panel — painted left column with gradient, logo and tagline
// ─────────────────────────────────────────────────────────────────────────────

class BrandingPanel : public QWidget {
    Q_OBJECT
public:
    explicit BrandingPanel(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedWidth(300);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);

        // ── Background gradient ──────────────────────────────────────────
        QLinearGradient bg(0, 0, 0, height());
        bg.setColorAt(0.00, QColor("#050510"));
        bg.setColorAt(0.40, QColor("#08082a"));
        bg.setColorAt(0.75, QColor("#060620"));
        bg.setColorAt(1.00, QColor("#030312"));
        p.fillRect(rect(), bg);

        // ── Subtle dot grid ──────────────────────────────────────────────
        p.setPen(QColor(80, 80, 160, 18));
        for (int gx = 0; gx < width(); gx += 24) {
            for (int gy = 0; gy < height(); gy += 24) {
                p.drawPoint(gx, gy);
            }
        }

        // ── Accent glow circles (decorative) ────────────────────────────
        auto drawGlow = [&](int cx, int cy, int radius, QColor col) {
            QRadialGradient glow(cx, cy, radius);
            col.setAlpha(55);
            glow.setColorAt(0.0, col);
            col.setAlpha(0);
            glow.setColorAt(1.0, col);
            p.setPen(Qt::NoPen);
            p.setBrush(glow);
            p.drawEllipse(cx - radius, cy - radius, radius * 2, radius * 2);
        };

        drawGlow(40,        90,       120, QColor("#4444cc"));
        drawGlow(width()-20, height()-80, 140, QColor("#226688"));
        drawGlow(width()/2,  height()/2,   80, QColor("#332266"));

        // ── Diamond logo mark ────────────────────────────────────────────
        const int cx   = width() / 2;
        const int logoY = 62;
        const int sz   = 36;

        // Outer glow
        QRadialGradient logoGlow(cx, logoY, sz + 20);
        logoGlow.setColorAt(0.0, QColor(100, 100, 255, 60));
        logoGlow.setColorAt(1.0, QColor(0,   0,   0,    0));
        p.setBrush(logoGlow);
        p.setPen(Qt::NoPen);
        p.drawEllipse(cx - sz - 20, logoY - sz - 20, (sz + 20) * 2, (sz + 20) * 2);

        // Diamond shape
        QPainterPath diamond;
        diamond.moveTo(cx,      logoY - sz);
        diamond.lineTo(cx + sz, logoY);
        diamond.lineTo(cx,      logoY + sz);
        diamond.lineTo(cx - sz, logoY);
        diamond.closeSubpath();

        QLinearGradient diaGrad(cx, logoY - sz, cx, logoY + sz);
        diaGrad.setColorAt(0.0, QColor("#9999ff"));
        diaGrad.setColorAt(0.5, QColor("#6666ee"));
        diaGrad.setColorAt(1.0, QColor("#4444aa"));
        p.setBrush(diaGrad);
        p.setPen(QPen(QColor("#aaaaff"), 1.2));
        p.drawPath(diamond);

        // Inner diamond (inverted smaller)
        QPainterPath innerDia;
        const int is = sz / 2;
        innerDia.moveTo(cx,      logoY - is);
        innerDia.lineTo(cx + is, logoY);
        innerDia.lineTo(cx,      logoY + is);
        innerDia.lineTo(cx - is, logoY);
        innerDia.closeSubpath();
        p.setBrush(QColor(5, 5, 20, 200));
        p.setPen(Qt::NoPen);
        p.drawPath(innerDia);

        // ── MOSAIC wordmark ──────────────────────────────────────────────
        QFont wordFont;
        wordFont.setPointSize(26);
        wordFont.setBold(true);
        wordFont.setLetterSpacing(QFont::AbsoluteSpacing, 5);
        p.setFont(wordFont);

        // Shadow
        p.setPen(QColor(20, 20, 80, 120));
        p.drawText(QRectF(2, logoY + sz + 18, width(), 40),
                   Qt::AlignHCenter | Qt::AlignTop, "MOSAIC");

        // Main text with gradient simulation (two-pass)
        p.setPen(QColor("#c8c8ff"));
        p.drawText(QRectF(0, logoY + sz + 16, width(), 40),
                   Qt::AlignHCenter | Qt::AlignTop, "MOSAIC");

        // ── Thin separator ────────────────────────────────────────────────
        const int sepY = logoY + sz + 68;
        QLinearGradient sepGrad(30, sepY, width() - 30, sepY);
        sepGrad.setColorAt(0.0, QColor(0, 0, 0, 0));
        sepGrad.setColorAt(0.4, QColor(80, 80, 160, 180));
        sepGrad.setColorAt(0.6, QColor(80, 80, 160, 180));
        sepGrad.setColorAt(1.0, QColor(0, 0, 0, 0));
        p.setPen(QPen(QBrush(sepGrad), 1.0));
        p.drawLine(30, sepY, width() - 30, sepY);

        // ── Subtitle text ─────────────────────────────────────────────────
        QFont subFont;
        subFont.setPointSize(8);
        p.setFont(subFont);
        p.setPen(QColor("#555588"));
        const QRectF subRect(20, sepY + 14, width() - 40, 80);
        p.drawText(subRect,
                   Qt::AlignHCenter | Qt::TextWordWrap,
                   "Multi-camera Observatory for Social & Activity Interaction Capture");

        // ── Feature tags ─────────────────────────────────────────────────
        const int tagsY = sepY + 110;
        auto drawTag = [&](int tagX, int tagY, const QString& text) {
            QFont tagFont;
            tagFont.setPointSize(7);
            tagFont.setBold(true);
            p.setFont(tagFont);
            const auto fm = p.fontMetrics();
            const int tw = fm.horizontalAdvance(text);
            const QRectF bg2(tagX - tw/2 - 7, tagY, tw + 14, 16);
            p.setBrush(QColor(20, 20, 60, 180));
            p.setPen(QPen(QColor("#2a2a66"), 1.0));
            p.drawRoundedRect(bg2, 3, 3);
            p.setPen(QColor("#6666aa"));
            p.drawText(bg2, Qt::AlignCenter, text);
        };

        drawTag(cx - 60, tagsY,     "Basler SDK");
        drawTag(cx + 56, tagsY,     "FFmpeg");
        drawTag(cx - 62, tagsY + 22, "Lab Streaming Layer");
        drawTag(cx + 52, tagsY + 22, "Qt6 C++23");

        // ── Version chip at bottom ────────────────────────────────────────
        QFont verFont;
        verFont.setPointSize(7);
        p.setFont(verFont);
        p.setPen(QColor("#2a2a4a"));
        p.drawText(QRectF(0, height() - 28, width(), 20),
                   Qt::AlignHCenter, "v0.1.0  ·  CSRU Laboratory");

        // ── Right edge divider line ───────────────────────────────────────
        QLinearGradient edgeGrad(width() - 1, 0, width() - 1, height());
        edgeGrad.setColorAt(0.0, QColor(0, 0, 0, 0));
        edgeGrad.setColorAt(0.3, QColor(60, 60, 160, 100));
        edgeGrad.setColorAt(0.7, QColor(60, 60, 160, 100));
        edgeGrad.setColorAt(1.0, QColor(0, 0, 0, 0));
        p.setPen(QPen(QBrush(edgeGrad), 1.0));
        p.drawLine(width() - 1, 0, width() - 1, height());
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Avatar chip — profile selection card
// IMPORTANT: stores Profile by value — the vector returned by profiles() is
// temporary and would make a reference dangle after rebuild_profile_grid() returns.
// ─────────────────────────────────────────────────────────────────────────────

class AvatarChip : public QWidget {
    Q_OBJECT
public:
    explicit AvatarChip(Profile profile, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_profile(std::move(profile))
    {
        setFixedSize(100, 124);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QString("%1\n@%2\n%3")
            .arg(m_profile.displayName,
                 m_profile.username,
                 m_profile.institution.isEmpty() ? "" : m_profile.institution));
    }

    void set_selected(bool selected) {
        if (m_selected == selected) { return; }
        m_selected = selected;
        update();
    }

    [[nodiscard]] QString username() const { return m_profile.username; }

signals:
    void clicked(QString username);

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);

        const int   diameter = 72;
        const int   circleX  = (width() - diameter) / 2;
        const QRectF circle(circleX, 8, diameter, diameter);
        const QColor accent(m_profile.accentColour.isEmpty()
                             ? "#5566bb" : m_profile.accentColour);

        // Hover / selected background card
        if (m_selected || underMouse()) {
            painter.setPen(Qt::NoPen);
            QColor cardBg = m_selected ? QColor(accent.red(), accent.green(), accent.blue(), 20)
                                       : QColor(255, 255, 255, 6);
            painter.setBrush(cardBg);
            painter.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 8, 8);
        }

        // Glow ring when selected
        if (m_selected) {
            QRadialGradient glow(circleX + diameter/2, 8 + diameter/2, diameter/2 + 10);
            glow.setColorAt(0.7, QColor(accent.red(), accent.green(), accent.blue(), 0));
            glow.setColorAt(0.85, QColor(accent.red(), accent.green(), accent.blue(), 50));
            glow.setColorAt(1.0, QColor(accent.red(), accent.green(), accent.blue(), 0));
            painter.setBrush(glow);
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(circle.adjusted(-10, -10, 10, 10));
        }

        // Main circle
        painter.setBrush(accent);
        painter.setPen(m_selected
                        ? QPen(QColor("#aaaaff"), 2.5)
                        : QPen(QColor("#1e1e3a"), 1.0));
        painter.drawEllipse(circle);

        // Initials
        QFont initFont;
        initFont.setPointSize(22);
        initFont.setBold(true);
        painter.setFont(initFont);
        painter.setPen(Qt::white);
        painter.drawText(circle.toRect(), Qt::AlignCenter,
                         m_profile.initials.isEmpty() ? "?" : m_profile.initials);

        // Selection checkmark badge
        if (m_selected) {
            const QRectF badge(circleX + diameter - 20, 8 + diameter - 20, 20, 20);
            painter.setBrush(QColor("#5555ee"));
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(badge);
            painter.setPen(QPen(Qt::white, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const QPointF tick[3] = {
                {badge.x() + 4,  badge.y() + 10},
                {badge.x() + 8,  badge.y() + 14},
                {badge.x() + 15, badge.y() + 6},
            };
            painter.drawPolyline(tick, 3);
        }

        // Admin crown badge (top-right)
        if (m_profile.is_admin()) {
            const QRectF crownBadge(circleX + diameter - 14, 4, 20, 20);
            QRadialGradient crownGlow(crownBadge.center(), 12);
            crownGlow.setColorAt(0.0, QColor(255, 200, 0, 80));
            crownGlow.setColorAt(1.0, QColor(255, 200, 0, 0));
            painter.setBrush(crownGlow);
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(crownBadge.adjusted(-4, -4, 4, 4));

            painter.setBrush(QColor("#1a1000"));
            painter.setPen(QPen(QColor("#aa7700"), 1.0));
            painter.drawEllipse(crownBadge);
            QFont crownFont;
            crownFont.setPixelSize(12);
            painter.setFont(crownFont);
            painter.setPen(QColor("#ffcc00"));
            painter.drawText(crownBadge, Qt::AlignCenter, "♚");
        }

        // Group name
        QFont groupFont;
        groupFont.setPointSize(8);
        groupFont.setBold(m_selected);
        painter.setFont(groupFont);
        painter.setPen(m_selected ? QColor("#d8d8ff") : QColor("#9090bb"));
        const QRectF groupRect(2, 85, width() - 4, 22);
        painter.drawText(groupRect,
                         Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                         m_profile.displayName.isEmpty()
                             ? m_profile.username : m_profile.displayName);

        // @username
        QFont userFont;
        userFont.setPointSize(7);
        painter.setFont(userFont);
        painter.setPen(QColor(m_selected ? "#6868aa" : "#484868"));
        painter.drawText(QRectF(0, 108, width(), 14),
                         Qt::AlignHCenter | Qt::AlignTop,
                         "@" + m_profile.username);
    }

    void mousePressEvent(QMouseEvent*) override { emit clicked(m_profile.username); }
    void enterEvent(QEnterEvent*)      override { update(); }
    void leaveEvent(QEvent*)           override { update(); }

private:
    Profile m_profile;
    bool    m_selected{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// Add-profile chip
// ─────────────────────────────────────────────────────────────────────────────

class AddChip : public QWidget {
    Q_OBJECT
public:
    explicit AddChip(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(100, 124);
        setCursor(Qt::PointingHandCursor);
        setToolTip("Create new profile");
    }

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const bool hovered = underMouse();
        if (hovered) {
            painter.setBrush(QColor(255, 255, 255, 8));
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 8, 8);
        }

        const QRectF circle(14, 8, 72, 72);
        painter.setBrush(hovered ? QColor("#161630") : QColor("#0e0e28"));
        painter.setPen(QPen(QColor(hovered ? "#4444aa" : "#252545"), 1.5, Qt::DashLine));
        painter.drawEllipse(circle);

        QFont font = painter.font();
        font.setPointSize(26);
        painter.setFont(font);
        painter.setPen(QColor(hovered ? "#7777ee" : "#33336a"));
        painter.drawText(circle, Qt::AlignCenter, "+");

        QFont nameFont;
        nameFont.setPointSize(8);
        painter.setFont(nameFont);
        painter.setPen(QColor(hovered ? "#6666aa" : "#44445a"));
        painter.drawText(QRectF(0, 85, width(), 22),
                         Qt::AlignHCenter | Qt::AlignTop, "New profile");
    }

    void mousePressEvent(QMouseEvent*) override { emit clicked(); }
    void enterEvent(QEnterEvent*)      override { update(); }
    void leaveEvent(QEvent*)           override { update(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// Impl
// ─────────────────────────────────────────────────────────────────────────────

struct LoginDialog::Impl {
    ProfileManager& profileMgr;
    QString         activeUsername;

    QStackedWidget* stack        = nullptr;
    QWidget*        loginPage    = nullptr;
    QWidget*        registerPage = nullptr;

    // Login page
    QGridLayout*       profileGrid     = nullptr;
    QWidget*           profileGridW    = nullptr;
    QLabel*            selectedLabel   = nullptr;
    QWidget*           passwordRow     = nullptr;
    QLineEdit*         passwordEdit    = nullptr;
    QLabel*            errorLabel      = nullptr;
    QPushButton*       loginBtn        = nullptr;
    QPushButton*       guestBtn        = nullptr;
    AvatarChip*        selectedChip    = nullptr;
    QList<AvatarChip*> chips;

    // Register page
    QLineEdit* regUsername  = nullptr;
    QLineEdit* regDisplay   = nullptr;
    QLineEdit* regPassword  = nullptr;
    QLineEdit* regConfirm   = nullptr;
    QCheckBox* regAdminCk   = nullptr;
    QWidget*   regAdminRow  = nullptr;
    QLabel*    regError     = nullptr;

    explicit Impl(ProfileManager& mgr) : profileMgr(mgr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// LoginDialog
// ─────────────────────────────────────────────────────────────────────────────

LoginDialog::LoginDialog(ProfileManager& profileMgr, QWidget* parent)
    : QDialog(parent, Qt::Window | Qt::FramelessWindowHint)
    , d(std::make_unique<Impl>(profileMgr))
{
    setModal(true);
    setMinimumSize(800, 520);
    resize(900, 560);
    setStyleSheet(R"(
        LoginDialog, QDialog { background: #08081a; }
        QLineEdit {
            background: #0a0a1f; border: 1px solid #252545;
            border-radius: 5px; padding: 8px 12px;
            color: #c8c8e0; font-size: 13px;
        }
        QLineEdit:focus { border-color: #6060ee; background: #0d0d25; }
        QPushButton[role="primary"] {
            background: #2828a0; border: 1px solid #4444cc; border-radius: 5px;
            padding: 9px 26px; color: #c8c8ff; font-size: 13px; font-weight: bold;
        }
        QPushButton[role="primary"]:hover  { background: #3030b8; border-color: #6060ee; }
        QPushButton[role="primary"]:pressed { background: #1c1c70; }
        QPushButton[role="primary"]:disabled {
            background: #12122a; border-color: #1e1e3a; color: #333355;
        }
        QPushButton[role="ghost"] {
            background: transparent; border: 1px solid #252545; border-radius: 5px;
            padding: 9px 18px; color: #6666aa; font-size: 12px;
        }
        QPushButton[role="ghost"]:hover { border-color: #44446a; color: #9999cc; background: #0d0d22; }
        QCheckBox { spacing: 8px; color: #8888aa; font-size: 12px; }
        QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid #333355;
            border-radius: 3px; background: #0a0a1f; }
        QCheckBox::indicator:checked { background: #4444aa; border-color: #6666cc; }
    )");

    build_ui();

    connect(&profileMgr, &ProfileManager::profiles_changed,
            this, &LoginDialog::rebuild_profile_grid);
}

LoginDialog::~LoginDialog() = default;

QString LoginDialog::active_username() const { return d->activeUsername; }

// ─────────────────────────────────────────────────────────────────────────────
// UI construction
// ─────────────────────────────────────────────────────────────────────────────

void LoginDialog::build_ui() {
    auto* mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(0, 0, 0, 0);
    mainLay->setSpacing(0);

    d->stack = new QStackedWidget;
    mainLay->addWidget(d->stack);

    // ── Login page: horizontal split ───────────────────────────────────────
    d->loginPage = new QWidget;
    auto* loginHLay = new QHBoxLayout(d->loginPage);
    loginHLay->setContentsMargins(0, 0, 0, 0);
    loginHLay->setSpacing(0);

    loginHLay->addWidget(new BrandingPanel(d->loginPage));  // Left panel

    // Right auth panel
    auto* authPanel = new QWidget;
    authPanel->setStyleSheet("QWidget { background: #08081a; }");
    auto* authLay = new QVBoxLayout(authPanel);
    authLay->setContentsMargins(36, 32, 36, 28);
    authLay->setSpacing(0);

    // Section label
    auto* whoLbl = new QLabel("SELECT YOUR RESEARCH GROUP");
    whoLbl->setStyleSheet("color: #38387a; font-size: 9px; font-weight: bold; letter-spacing: 2px;");
    authLay->addWidget(whoLbl);

    authLay->addSpacing(14);

    // Profile grid (scrollable)
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setStyleSheet(
        "QScrollArea { background: transparent; border: none; }"
        "QScrollBar:vertical { background: #08081a; width: 6px; border-radius: 3px; }"
        "QScrollBar::handle:vertical { background: #2a2a55; border-radius: 3px; min-height: 20px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }");
    scroll->setMinimumHeight(140);
    scroll->setMaximumHeight(280);

    d->profileGridW = new QWidget;
    d->profileGridW->setStyleSheet("background: transparent;");
    d->profileGrid  = new QGridLayout(d->profileGridW);
    d->profileGrid->setContentsMargins(0, 0, 0, 0);
    d->profileGrid->setSpacing(8);
    scroll->setWidget(d->profileGridW);
    authLay->addWidget(scroll);

    authLay->addSpacing(14);

    // Selected profile label
    d->selectedLabel = new QLabel("Click a profile to select it");
    d->selectedLabel->setStyleSheet("color: #44446a; font-size: 11px;");
    authLay->addWidget(d->selectedLabel);

    authLay->addSpacing(10);

    // Password row (hidden initially)
    d->passwordRow = new QWidget;
    auto* passLay  = new QHBoxLayout(d->passwordRow);
    passLay->setContentsMargins(0, 0, 0, 0);
    passLay->setSpacing(8);
    auto* passIcon = new QLabel("🔒");
    passIcon->setFixedWidth(22);
    passLay->addWidget(passIcon);
    d->passwordEdit = new QLineEdit;
    d->passwordEdit->setPlaceholderText("Password");
    d->passwordEdit->setEchoMode(QLineEdit::Password);
    connect(d->passwordEdit, &QLineEdit::returnPressed,
            this, &LoginDialog::on_password_return_pressed);
    passLay->addWidget(d->passwordEdit, 1);
    d->passwordRow->setVisible(false);
    authLay->addWidget(d->passwordRow);

    // Error label
    d->errorLabel = new QLabel;
    d->errorLabel->setStyleSheet("color: #dd4444; font-size: 11px;");
    d->errorLabel->setVisible(false);
    authLay->addWidget(d->errorLabel);

    authLay->addSpacing(10);

    // Divider
    auto* divider = new QFrame;
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet("QFrame { background: #141430; border: none; max-height: 1px; }");
    authLay->addWidget(divider);

    authLay->addSpacing(14);

    // Bottom button row
    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(10);

    d->guestBtn = new QPushButton("Continue as guest");
    d->guestBtn->setProperty("role", "ghost");
    connect(d->guestBtn, &QPushButton::clicked, this, &LoginDialog::on_guest_clicked);
    btnRow->addWidget(d->guestBtn);

    btnRow->addStretch();

    d->loginBtn = new QPushButton("▶  Continue");
    d->loginBtn->setProperty("role", "primary");
    d->loginBtn->setEnabled(false);
    connect(d->loginBtn, &QPushButton::clicked, this, &LoginDialog::on_login_clicked);
    btnRow->addWidget(d->loginBtn);

    authLay->addLayout(btnRow);
    authLay->addStretch();

    // "Create admin account" link — only shown when no admin exists
    if (!d->profileMgr.has_admin()) {
        auto* adminRow = new QHBoxLayout;
        auto* adminLink = new QPushButton("⚙  Set up admin account…");
        adminLink->setStyleSheet(
            "QPushButton { background: transparent; border: none; "
            "color: #333358; font-size: 10px; text-decoration: underline; }"
            "QPushButton:hover { color: #555588; }");
        adminLink->setFlat(true);
        connect(adminLink, &QPushButton::clicked, this, [this] {
            d->regAdminRow->setVisible(true);
            show_register_mode();
        });
        adminRow->addStretch();
        adminRow->addWidget(adminLink);
        authLay->addLayout(adminRow);
    }

    loginHLay->addWidget(authPanel, 1);

    d->stack->addWidget(d->loginPage);   // index 0

    // ── Register page ──────────────────────────────────────────────────────
    d->registerPage = new QWidget;
    d->registerPage->setStyleSheet("QWidget { background: #08081a; }");
    auto* regOuter = new QHBoxLayout(d->registerPage);
    regOuter->setContentsMargins(0, 0, 0, 0);
    regOuter->setSpacing(0);

    regOuter->addWidget(new BrandingPanel(d->registerPage));

    auto* regPanel = new QWidget;
    auto* regLay   = new QVBoxLayout(regPanel);
    regLay->setContentsMargins(36, 32, 36, 28);
    regLay->setSpacing(0);

    auto* regTitle = new QLabel("Create a new research-group profile");
    regTitle->setStyleSheet("color: #c8c8e0; font-size: 15px; font-weight: bold;");
    regLay->addWidget(regTitle);

    regLay->addSpacing(6);
    auto* regSub = new QLabel("Each group gets its own camera configuration and recording settings.");
    regSub->setStyleSheet("color: #38386a; font-size: 11px;");
    regSub->setWordWrap(true);
    regLay->addWidget(regSub);

    regLay->addSpacing(22);

    auto make_field = [&](const QString& placeholder, bool password = false) -> QLineEdit* {
        auto* edit = new QLineEdit;
        edit->setPlaceholderText(placeholder);
        if (password) { edit->setEchoMode(QLineEdit::Password); }
        return edit;
    };

    auto make_row = [&](const QString& label, QLineEdit* field) {
        auto* row = new QHBoxLayout;
        auto* lbl = new QLabel(label);
        lbl->setStyleSheet("color: #7070a0; font-size: 11px;");
        lbl->setFixedWidth(100);
        lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(lbl);
        row->addSpacing(10);
        row->addWidget(field, 1);
        regLay->addLayout(row);
        regLay->addSpacing(10);
    };

    d->regUsername = make_field("e.g. cognitive_lab  (3–32 chars, a-z 0-9 _)");
    d->regDisplay  = make_field("e.g. Cognitive Science Lab");
    d->regPassword = make_field("Leave blank for no password", true);
    d->regConfirm  = make_field("Confirm password", true);

    make_row("Username:", d->regUsername);
    make_row("Group name:", d->regDisplay);
    make_row("Password:", d->regPassword);
    make_row("Confirm:", d->regConfirm);

    // Admin registration row (hidden by default, shown via the setup link)
    d->regAdminRow = new QWidget;
    d->regAdminRow->setVisible(false);
    auto* adminRowLay = new QHBoxLayout(d->regAdminRow);
    adminRowLay->setContentsMargins(0, 0, 0, 0);
    adminRowLay->addSpacing(110);
    adminRowLay->addSpacing(10);
    d->regAdminCk = new QCheckBox("Register as admin (can manage all profiles)");
    d->regAdminCk->setStyleSheet(d->regAdminCk->styleSheet() +
        " QCheckBox { color: #ccaa44; }");
    adminRowLay->addWidget(d->regAdminCk, 1);
    regLay->addWidget(d->regAdminRow);
    regLay->addSpacing(6);

    d->regError = new QLabel;
    d->regError->setStyleSheet("color: #dd4444; font-size: 11px;");
    d->regError->setVisible(false);
    regLay->addWidget(d->regError);

    regLay->addStretch();

    auto* regDivider = new QFrame;
    regDivider->setFrameShape(QFrame::HLine);
    regDivider->setStyleSheet("QFrame { background: #141430; border: none; max-height: 1px; }");
    regLay->addWidget(regDivider);
    regLay->addSpacing(14);

    auto* regBtnRow = new QHBoxLayout;
    regBtnRow->setSpacing(10);

    auto* cancelBtn = new QPushButton("← Back");
    cancelBtn->setProperty("role", "ghost");
    connect(cancelBtn, &QPushButton::clicked, this, &LoginDialog::on_cancel_register_clicked);
    regBtnRow->addWidget(cancelBtn);

    regBtnRow->addStretch();

    auto* createBtn = new QPushButton("Create profile  ▶");
    createBtn->setStyleSheet(
        "QPushButton { background: #1a5a28; border: 1px solid #33aa55; border-radius: 5px;"
        "  padding: 9px 24px; color: #88ffaa; font-size: 13px; font-weight: bold; }"
        "QPushButton:hover { background: #22703a; border-color: #44cc66; }"
        "QPushButton:pressed { background: #124020; }");
    connect(createBtn, &QPushButton::clicked, this, &LoginDialog::on_register_clicked);
    regBtnRow->addWidget(createBtn);

    regLay->addLayout(regBtnRow);

    regOuter->addWidget(regPanel, 1);

    d->stack->addWidget(d->registerPage); // index 1

    rebuild_profile_grid();
}

// ─────────────────────────────────────────────────────────────────────────────
// Profile grid
// ─────────────────────────────────────────────────────────────────────────────

void LoginDialog::rebuild_profile_grid() {
    for (auto* chip : d->chips) { chip->deleteLater(); }
    d->chips.clear();

    while (d->profileGrid->count()) {
        auto* item = d->profileGrid->takeAt(0);
        delete item;
    }
    d->selectedChip = nullptr;

    const std::vector<Profile> profiles = d->profileMgr.profiles();
    constexpr int kCols = 4;

    int col = 0;
    int row = 0;

    for (const Profile& prof : profiles) {
        auto* chip = new AvatarChip(prof, d->profileGridW);
        connect(chip, &AvatarChip::clicked, this, &LoginDialog::on_card_selected);
        d->profileGrid->addWidget(chip, row, col);
        d->chips.append(chip);
        ++col;
        if (col >= kCols) { col = 0; ++row; }
    }

    auto* addChip = new AddChip(d->profileGridW);
    connect(addChip, &AddChip::clicked, this, [this] {
        d->regAdminRow->setVisible(false);
        show_register_mode();
    });
    d->profileGrid->addWidget(addChip, row, col);

    if (!d->activeUsername.isEmpty()) {
        on_card_selected(d->activeUsername);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Login mode
// ─────────────────────────────────────────────────────────────────────────────

void LoginDialog::show_login_mode() {
    d->stack->setCurrentIndex(0);
}

void LoginDialog::on_card_selected(const QString& username) {
    for (auto* chip : d->chips) {
        chip->set_selected(chip->username() == username);
        if (chip->username() == username) { d->selectedChip = chip; }
    }

    d->activeUsername = username;
    clear_error();

    const Profile* prof = d->profileMgr.find(username);
    if (!prof) { return; }

    QString label = QString("<b style='color:#9999dd'>%1</b>")
        .arg(prof->displayName.toHtmlEscaped());
    if (!prof->institution.isEmpty()) {
        label += QString("  ·  <span style='color:#555577'>%1</span>")
            .arg(prof->institution.toHtmlEscaped());
    }
    if (prof->is_admin()) {
        label += "  <span style='color:#ffcc00; font-size:10px;'>♚ Admin</span>";
    }
    d->selectedLabel->setText(label);

    const bool needsPassword = prof->has_password();
    d->passwordRow->setVisible(needsPassword);
    if (needsPassword) {
        d->passwordEdit->clear();
        d->passwordEdit->setFocus();
    }

    d->loginBtn->setEnabled(true);
}

void LoginDialog::slide_password_in(bool visible) {
    d->passwordRow->setVisible(visible);
    if (visible) { d->passwordEdit->setFocus(); }
}

// ─────────────────────────────────────────────────────────────────────────────
// Register mode
// ─────────────────────────────────────────────────────────────────────────────

void LoginDialog::show_register_mode() {
    d->regUsername->clear();
    d->regDisplay->clear();
    d->regPassword->clear();
    d->regConfirm->clear();
    if (d->regAdminCk) { d->regAdminCk->setChecked(false); }
    d->regError->hide();
    d->stack->setCurrentIndex(1);
    d->regUsername->setFocus();
}

void LoginDialog::on_cancel_register_clicked() {
    show_login_mode();
}

void LoginDialog::on_register_clicked() {
    const QString username = d->regUsername->text().trimmed().toLower();
    const QString display  = d->regDisplay->text().trimmed();
    const QString password = d->regPassword->text();
    const QString confirm  = d->regConfirm->text();
    const bool    asAdmin  = d->regAdminCk && d->regAdminCk->isChecked()
                             && d->regAdminCk->isVisible();

    if (!password.isEmpty() && password != confirm) {
        d->regError->setText("Passwords do not match.");
        d->regError->show();
        return;
    }

    const Profile::Role role = asAdmin ? Profile::Role::Admin : Profile::Role::User;
    using Res = ProfileManager::RegisterResult;
    const auto result = d->profileMgr.register_profile(username, display, password, role);

    switch (result) {
    case Res::Ok:
        d->profileMgr.touch(username);
        d->activeUsername = username;
        rebuild_profile_grid();
        show_login_mode();
        on_card_selected(username);
        accept();
        break;
    case Res::UsernameTaken:
        d->regError->setText(QString("Username '%1' is already taken.").arg(username));
        d->regError->show();
        break;
    case Res::UsernameInvalid:
        d->regError->setText("Username must be 3–32 characters: a-z, 0-9, _ only.");
        d->regError->show();
        break;
    case Res::PasswordTooShort:
        d->regError->setText("Password must be at least 4 characters (or leave blank).");
        d->regError->show();
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Authentication
// ─────────────────────────────────────────────────────────────────────────────

void LoginDialog::attempt_login() {
    if (d->activeUsername.isEmpty()) {
        set_error("Please select a profile.");
        return;
    }

    const Profile* prof = d->profileMgr.find(d->activeUsername);
    if (!prof) { return; }

    if (prof->has_password()) {
        const QString pw = d->passwordEdit->text();
        if (!d->profileMgr.verify(d->activeUsername, pw)) {
            set_error("Incorrect password.");
            d->passwordEdit->selectAll();
            d->passwordEdit->setFocus();
            return;
        }
    }

    d->profileMgr.touch(d->activeUsername);
    accept();
}

void LoginDialog::on_login_clicked()           { attempt_login(); }
void LoginDialog::on_password_return_pressed() { attempt_login(); }

void LoginDialog::on_guest_clicked() {
    d->activeUsername = "guest";
    accept();
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

void LoginDialog::set_error(const QString& msg) {
    d->errorLabel->setText(msg);
    d->errorLabel->setVisible(true);
}

void LoginDialog::clear_error() {
    d->errorLabel->setVisible(false);
}

void LoginDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) { return; }
    QDialog::keyPressEvent(event);
}

} // namespace mosaic

#include "login_dialog.moc"
