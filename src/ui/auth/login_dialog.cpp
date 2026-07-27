#include "ui/auth/login_dialog.hpp"
#include "ui/anim_utils.hpp"
#include <QCheckBox>
#include <QEasingCurve>
#include <QFont>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QParallelAnimationGroup>
#include <QPointer>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QVariantAnimation>
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

        // Slow, constant-speed spin of the diamond mark — the first
        // continuous/looping animation in this codebase (everything else is
        // a one-shot discrete transition). Only started/stopped in
        // showEvent()/hideEvent() below so it doesn't keep repainting while
        // this panel (and the login dialog it lives in) isn't on screen.
        m_rotationAnim = new QVariantAnimation(this);
        m_rotationAnim->setStartValue(0.0);
        m_rotationAnim->setEndValue(360.0);
        m_rotationAnim->setDuration(24000);
        m_rotationAnim->setEasingCurve(QEasingCurve::Linear);
        m_rotationAnim->setLoopCount(-1);
        connect(m_rotationAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            m_diamondAngle = v.toDouble();
            update();
        });

        // Subtle "breathing" glow behind the diamond, same start/stop gating.
        m_pulseAnim = new QVariantAnimation(this);
        m_pulseAnim->setKeyValueAt(0.0, 0.85);
        m_pulseAnim->setKeyValueAt(0.5, 1.15);
        m_pulseAnim->setKeyValueAt(1.0, 0.85);
        m_pulseAnim->setDuration(3200);
        m_pulseAnim->setEasingCurve(QEasingCurve::InOutSine);
        m_pulseAnim->setLoopCount(-1);
        connect(m_pulseAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            m_glowPulse = v.toDouble();
            update();
        });
    }

protected:
    void showEvent(QShowEvent* event) override {
        QWidget::showEvent(event);
        m_rotationAnim->start();
        m_pulseAnim->start();
    }

    void hideEvent(QHideEvent* event) override {
        QWidget::hideEvent(event);
        m_rotationAnim->stop();
        m_pulseAnim->stop();
    }

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

        // ── Diamond logo mark (rotating; wordmark/tags below stay static) ──
        const int cx   = width() / 2;
        const int logoY = 62;
        const int sz   = 36;

        p.save();
        p.translate(cx, logoY);
        p.rotate(m_diamondAngle);
        p.translate(-cx, -logoY);

        // Outer glow (subtle breathing pulse)
        const qreal glowR = (sz + 20) * m_glowPulse;
        QRadialGradient logoGlow(QPointF(cx, logoY), glowR);
        logoGlow.setColorAt(0.0, QColor(100, 100, 255, 60));
        logoGlow.setColorAt(1.0, QColor(0,   0,   0,    0));
        p.setBrush(logoGlow);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(cx, logoY), glowR, glowR);

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

        p.restore();

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

private:
    qreal m_diamondAngle = 0.0;
    qreal m_glowPulse    = 1.0;
    QVariantAnimation* m_rotationAnim = nullptr;
    QVariantAnimation* m_pulseAnim    = nullptr;
};

// Small animation/paint helpers shared across the UI (ensure_opacity_effect,
// fade_in_widget, shake_widget, lerp_color, restart_hover_anim,
// crossfade_stacked_widget, ...) live in mosaic::anim (ui/anim_utils.hpp) —
// promoted there once other widgets needed the same recipes this file
// originated.

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

        m_hoverAnim = new QVariantAnimation(this);
        m_hoverAnim->setDuration(120);
        m_hoverAnim->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_hoverAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            m_hoverT = v.toDouble();
            update();
        });
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

        // Hover / selected background card (hover fades smoothly via
        // m_hoverT; the selected state itself stays a hard on/off, since it
        // shouldn't fade with hover)
        if (m_selected || m_hoverT > 0.0) {
            painter.setPen(Qt::NoPen);
            QColor cardBg = m_selected
                ? QColor(accent.red(), accent.green(), accent.blue(), 20)
                : QColor(255, 255, 255, static_cast<int>(6 * m_hoverT));
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
    void enterEvent(QEnterEvent*)      override { anim::restart_hover_anim(m_hoverAnim, m_hoverT, 1.0); }
    void leaveEvent(QEvent*)           override { anim::restart_hover_anim(m_hoverAnim, m_hoverT, 0.0); }

private:
    Profile m_profile;
    bool    m_selected{false};
    qreal   m_hoverT{0.0};
    QVariantAnimation* m_hoverAnim = nullptr;
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

        m_hoverAnim = new QVariantAnimation(this);
        m_hoverAnim->setDuration(120);
        m_hoverAnim->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_hoverAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            m_hoverT = v.toDouble();
            update();
        });
    }

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        if (m_hoverT > 0.0) {
            painter.setBrush(QColor(255, 255, 255, static_cast<int>(8 * m_hoverT)));
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 8, 8);
        }

        const QRectF circle(14, 8, 72, 72);
        painter.setBrush(anim::lerp_color(QColor("#0e0e28"), QColor("#161630"), m_hoverT));
        painter.setPen(QPen(anim::lerp_color(QColor("#252545"), QColor("#4444aa"), m_hoverT),
                             1.5, Qt::DashLine));
        painter.drawEllipse(circle);

        QFont font = painter.font();
        font.setPointSize(26);
        painter.setFont(font);
        painter.setPen(anim::lerp_color(QColor("#33336a"), QColor("#7777ee"), m_hoverT));
        painter.drawText(circle, Qt::AlignCenter, "+");

        QFont nameFont;
        nameFont.setPointSize(8);
        painter.setFont(nameFont);
        painter.setPen(anim::lerp_color(QColor("#44445a"), QColor("#6666aa"), m_hoverT));
        painter.drawText(QRectF(0, 85, width(), 22),
                         Qt::AlignHCenter | Qt::AlignTop, "New profile");
    }

    void mousePressEvent(QMouseEvent*) override { emit clicked(); }
    void enterEvent(QEnterEvent*)      override { anim::restart_hover_anim(m_hoverAnim, m_hoverT, 1.0); }
    void leaveEvent(QEvent*)           override { anim::restart_hover_anim(m_hoverAnim, m_hoverT, 0.0); }

private:
    qreal m_hoverT{0.0};
    QVariantAnimation* m_hoverAnim = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
// Impl
// ─────────────────────────────────────────────────────────────────────────────

struct LoginDialog::Impl {
    ProfileManager& profileMgr;
    QString         activeUsername;
    bool            hasFadedIn               = false; // dialog fade-in runs once
    bool            passwordRowTargetVisible = false; // guards redundant slide_password_in() restarts
    // crossfade_to_page()'s own re-entrancy-guard state (target page index +
    // in-flight animation) now lives inside anim::crossfade_stacked_widget's
    // internal side-table, keyed by `stack` — nothing to track here anymore.
    //
    // Tracks whichever animation is currently in flight for
    // slide_password_in()'s multi-stage transition, so a re-entrant call
    // (e.g. fast repeated profile switching) can cancel the stale one
    // instead of leaving two animations racing on the same property.
    QPointer<QParallelAnimationGroup> passwordAnimGroup;

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
    AddChip*           addChip         = nullptr; // rebuilt each time; previous one must be deleted

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
    // takeAt()+delete below only removes the QLayoutItem wrapper, not the
    // widget it wraps — the "+" card must be explicitly deleted too, same
    // as the avatar chips above, or it's silently orphaned (still a live
    // child widget with its own running hover animation) on every rebuild.
    if (d->addChip) { d->addChip->deleteLater(); d->addChip = nullptr; }

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

    d->addChip = new AddChip(d->profileGridW);
    connect(d->addChip, &AddChip::clicked, this, [this] {
        d->regAdminRow->setVisible(false);
        show_register_mode();
    });
    d->profileGrid->addWidget(d->addChip, row, col);

    if (!d->activeUsername.isEmpty()) {
        on_card_selected(d->activeUsername);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Login mode
// ─────────────────────────────────────────────────────────────────────────────

// Sequential fade-out/fade-in page switch — QStackedWidget only ever paints
// one page at a time, so a true overlapping crossfade isn't possible without
// restructuring away from it; this reads as a smooth transition rather than
// today's hard cut, which is the actual goal. Thin wrapper around the shared
// anim::crossfade_stacked_widget() (see ui/anim_utils.hpp) — kept as a named
// member since show_login_mode()/show_register_mode() and this class's own
// header doc comment already describe it in login-specific terms (index 0 =
// login, 1 = register).
void LoginDialog::crossfade_to_page(int index, std::function<void()> onComplete) {
    anim::crossfade_stacked_widget(d->stack, index, 130, std::move(onComplete));
}

void LoginDialog::show_login_mode() {
    crossfade_to_page(0);
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
    slide_password_in(needsPassword);

    d->loginBtn->setEnabled(true);
}

// Grows/fades the password row in or out (reuses CameraCardW::toggle_expanded()'s
// exact maximumHeight/180ms/InOutCubic recipe, plus an opacity fade so it both
// grows and fades together instead of popping instantly).
void LoginDialog::slide_password_in(bool visible) {
    if (visible == d->passwordRowTargetVisible) {
        // Row is already at (or already animating toward) this visibility,
        // so there's no reveal/collapse animation to (re)start. But when
        // switching directly between two password-protected profiles, the
        // field itself must still reset for the newly-selected profile —
        // otherwise it would silently keep showing the previous profile's
        // leftover text, unfocused.
        if (visible) {
            d->passwordEdit->clear();
            d->passwordEdit->setFocus();
        }
        return;
    }
    d->passwordRowTargetVisible = visible;

    // Cancel a still-in-flight reveal/collapse before starting a new one —
    // switching between profiles quickly enough to retrigger this mid-
    // animation would otherwise leave two animations racing on the same
    // maximumHeight/opacity properties (visible jank, and a final state that
    // depends on whichever one happens to finish last).
    if (d->passwordAnimGroup) { d->passwordAnimGroup->stop(); }

    auto* opacityEffect = anim::ensure_opacity_effect(d->passwordRow);

    auto* heightAnim = new QPropertyAnimation(d->passwordRow, "maximumHeight");
    heightAnim->setDuration(180);
    heightAnim->setEasingCurve(QEasingCurve::InOutCubic);

    auto* opacityAnim = new QPropertyAnimation(opacityEffect, "opacity");
    opacityAnim->setDuration(180);
    opacityAnim->setEasingCurve(QEasingCurve::InOutCubic);

    if (visible) {
        d->passwordRow->show();
        d->passwordRow->setMaximumHeight(0);
        opacityEffect->setOpacity(0.0);
        heightAnim->setStartValue(0);
        heightAnim->setEndValue(d->passwordRow->sizeHint().height());
        opacityAnim->setStartValue(0.0);
        opacityAnim->setEndValue(1.0);
        connect(heightAnim, &QAbstractAnimation::finished, d->passwordRow, [this] {
            d->passwordRow->setMaximumHeight(QWIDGETSIZE_MAX);
        });
        d->passwordEdit->clear();
        d->passwordEdit->setFocus();
    } else {
        heightAnim->setStartValue(d->passwordRow->height());
        heightAnim->setEndValue(0);
        opacityAnim->setStartValue(opacityEffect->opacity());
        opacityAnim->setEndValue(0.0);
        connect(heightAnim, &QAbstractAnimation::finished, d->passwordRow, &QWidget::hide);
    }

    auto* group = new QParallelAnimationGroup(d->passwordRow);
    group->addAnimation(heightAnim);
    group->addAnimation(opacityAnim);
    d->passwordAnimGroup = group;
    group->start(QAbstractAnimation::DeleteWhenStopped);
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
    // setFocus() must wait until the register page is actually current —
    // crossfade_to_page() defers that ~130ms behind its fade-out animation,
    // and a focus request against a still-hidden page's widget is not
    // reliably preserved until it becomes visible.
    crossfade_to_page(1, [this] { d->regUsername->setFocus(); });
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
        set_register_error("Passwords do not match.");
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
        set_register_error(QString("Username '%1' is already taken.").arg(username));
        break;
    case Res::UsernameInvalid:
        set_register_error("Username must be 3–32 characters: a-z, 0-9, _ only.");
        break;
    case Res::PasswordTooShort:
        set_register_error("Password must be at least 4 characters (or leave blank).");
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
    anim::fade_in_widget(d->errorLabel);
    anim::shake_widget(d->passwordRow->isVisible() ? d->passwordRow : d->profileGridW);
}

void LoginDialog::clear_error() {
    d->errorLabel->setVisible(false);
}

void LoginDialog::set_register_error(const QString& msg) {
    d->regError->setText(msg);
    d->regError->setVisible(true);
    anim::fade_in_widget(d->regError);
    anim::shake_widget(d->regError->parentWidget());
}

void LoginDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) { return; }
    QDialog::keyPressEvent(event);
}

// Gentle fade-in the first (and only, in practice — a fresh LoginDialog is
// constructed each time through main.cpp's auth loop) time this dialog is
// shown.
void LoginDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (d->hasFadedIn) { return; }
    d->hasFadedIn = true;

    auto* effect = anim::ensure_opacity_effect(this);
    effect->setOpacity(0.0);
    auto* anim = new QPropertyAnimation(effect, "opacity", this);
    anim->setDuration(220);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

} // namespace mosaic

#include "login_dialog.moc"
