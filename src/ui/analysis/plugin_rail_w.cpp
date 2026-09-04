#include "ui/analysis/plugin_rail_w.hpp"

#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <algorithm>
#include <functional>
#include <utility>

#include "analysis/analysis_plugins.hpp"
#include "ui/anim_utils.hpp"

namespace mosaic {

namespace {

// Two lines — label above blurb — plus breathing room.
constexpr int kRowHeight = 46;
// Left inset shared by the label, the blurb and the category headings, so the
// three line up down the rail.
constexpr int kTextLeft = 14;
// Run-state dot geometry, measured in from the right edge.
constexpr int kDotRight  = 16;
constexpr int kDotRadius = 4;

// One selectable plugin. A plain QWidget with a hand-rolled paintEvent, not a
// QListWidgetItem: the same shape SessionRow uses, and it takes a
// std::function callback rather than a signal so the row needs no Q_OBJECT
// (and so this file needs no .moc include).
class PluginRowW : public QWidget {
   public:
    using ClickCb = std::function<void(const QString&)>;
    /// Moves focus to the row `delta` positions away, clamped at the ends.
    /// Supplied by the rail, which is the only thing that knows the order.
    using FocusNeighbourCb = std::function<void(const PluginRowW*, int)>;

    PluginRowW(const AnalysisPluginDesc& desc, ClickCb onClick, FocusNeighbourCb onFocusNeighbour,
               QWidget* parent = nullptr)
        : QWidget(parent),
          m_desc(desc),
          m_onClick(std::move(onClick)),
          m_onFocusNeighbour(std::move(onFocusNeighbour)) {
        setFixedHeight(kRowHeight);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
        // A QComboBox was keyboard-reachable and screen-reader-labelled for
        // free. Hand-rolled rows are not, so this is a regression to avoid
        // rather than a nicety: without it the picker becomes mouse-only.
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(desc.label);
        setAccessibleDescription(desc.blurb);
        setToolTip(desc.blurb);

        m_hoverAnim = new QVariantAnimation(this);
        m_hoverAnim->setDuration(120);
        m_hoverAnim->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_hoverAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            m_hoverT = v.toReal();
            update();
        });
    }

    void set_selected(bool selected) {
        if (m_selected == selected) {
            return;
        }
        m_selected = selected;
        update();
    }

    void set_run_state(PluginRunState state) {
        if (m_runState == state) {
            return;
        }
        m_runState = state;
        setToolTip(m_desc.blurb + run_state_note());
        update();
    }

    [[nodiscard]] const QString& plugin_id() const { return m_desc.id; }

   protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // Same palette and hover lerp the session list beside this rail uses,
        // so the two columns read as one control surface.
        const QColor bg = m_selected
                              ? QColor("#3a3a88")
                              : anim::lerp_color(QColor("#13132a"), QColor("#1a1a38"), m_hoverT);
        p.fillRect(rect(), bg);

        if (m_selected) {
            p.fillRect(0, 0, 3, height(), QColor("#6060dd"));
        } else if (hasFocus()) {
            // Keyboard focus needs to be visible without looking selected.
            p.fillRect(0, 0, 3, height(), QColor("#3a3a6a"));
        }

        const int textWidth = width() - kTextLeft - kDotRight - 8;

        QFont labelFont;
        labelFont.setPixelSize(13);
        labelFont.setBold(true);
        p.setFont(labelFont);
        p.setPen(QColor("#c8c8e0"));
        // Elided, not wrapped: the row height is fixed, and several labels are
        // longer than a narrow rail.
        p.drawText(QRect(kTextLeft, 5, textWidth, 18), Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(labelFont).elidedText(m_desc.label, Qt::ElideRight, textWidth));

        QFont blurbFont;
        blurbFont.setPixelSize(10);
        p.setFont(blurbFont);
        p.setPen(QColor(m_selected ? "#a8a8d8" : "#5858a0"));
        p.drawText(QRect(kTextLeft, 24, textWidth, 15), Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(blurbFont).elidedText(m_desc.blurb, Qt::ElideRight, textWidth));

        paint_run_state(p);

        p.setPen(QColor("#1e1e3a"));
        p.drawLine(0, height() - 1, width(), height() - 1);
    }

    void mousePressEvent(QMouseEvent* event) override {
        // Left button only. A right-click here should not change the
        // selection — it would be an invisible way to lose the current view.
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        setFocus(Qt::MouseFocusReason);
        m_onClick(m_desc.id);
    }

    void keyPressEvent(QKeyEvent* event) override {
        switch (event->key()) {
            case Qt::Key_Space:
            case Qt::Key_Return:
            case Qt::Key_Enter:
                m_onClick(m_desc.id);
                return;
            // Explicitly between rows, not focusNextPrevChild(): that walks the
            // whole window's focus chain, so Down on the last plugin would
            // land in the results panel and Up on the first in the session
            // list — and arrow keys could never bring focus back.
            case Qt::Key_Up:
                m_onFocusNeighbour(this, -1);
                return;
            case Qt::Key_Down:
                m_onFocusNeighbour(this, 1);
                return;
            default:
                QWidget::keyPressEvent(event);
        }
    }

    void enterEvent(QEnterEvent*) override { anim::restart_hover_anim(m_hoverAnim, m_hoverT, 1.0); }
    void leaveEvent(QEvent*) override { anim::restart_hover_anim(m_hoverAnim, m_hoverT, 0.0); }
    void focusInEvent(QFocusEvent* e) override {
        QWidget::focusInEvent(e);
        update();
    }
    void focusOutEvent(QFocusEvent* e) override {
        QWidget::focusOutEvent(e);
        update();
    }

   private:
    void paint_run_state(QPainter& p) const {
        // Unknown draws nothing at all. An empty slot is honest about having
        // no information; any glyph would be read as "not run".
        if (m_runState == PluginRunState::Unknown) {
            return;
        }
        const QPoint centre(width() - kDotRight, height() / 2);
        if (m_runState == PluginRunState::None) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor("#2a2a50"), 1));
        } else {
            // Amber and green are this tab's existing "running" and "done"
            // status colours.
            p.setBrush(QColor(m_runState == PluginRunState::Complete ? "#44cc66" : "#ddaa33"));
            p.setPen(Qt::NoPen);
        }
        p.drawEllipse(centre, kDotRadius, kDotRadius);
    }

    [[nodiscard]] QString run_state_note() const {
        switch (m_runState) {
            case PluginRunState::Unknown:
                return {};
            case PluginRunState::None:
                return "\n\nNot yet run for this session.";
            case PluginRunState::Partial:
                return "\n\nPartly run for this session — some cameras are missing output, or "
                       "the existing output came from a different model or backend than the one "
                       "currently selected.";
            case PluginRunState::Complete:
                return "\n\nAlready run for this session, with the current settings.";
        }
        return {};
    }

    AnalysisPluginDesc m_desc;
    ClickCb m_onClick;
    FocusNeighbourCb m_onFocusNeighbour;
    bool m_selected                = false;
    PluginRunState m_runState      = PluginRunState::Unknown;
    QVariantAnimation* m_hoverAnim = nullptr;
    qreal m_hoverT                 = 0.0;
};

} // namespace

struct AnalysisPluginRailW::Impl {
    QVector<PluginRowW*> rows;
    QString current;
};

AnalysisPluginRailW::AnalysisPluginRailW(const QStringList& availableIds, QWidget* parent)
    : QWidget(parent), d(std::make_unique<Impl>()) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(4);

    // Mirrors the "Sessions" heading on the neighbouring panel.
    outer->addWidget(new QLabel("<b>Analysis</b>"));

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content = new QWidget;
    auto* lay     = new QVBoxLayout(content);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto on_click = [this](const QString& id) {
        set_current(id);
        emit plugin_selected(id);
    };
    auto on_focus_neighbour = [this](const PluginRowW* from, int delta) {
        const int index = d->rows.indexOf(const_cast<PluginRowW*>(from));
        if (index < 0) {
            return;
        }
        const int target = std::clamp(index + delta, 0, static_cast<int>(d->rows.size()) - 1);
        if (target != index) {
            d->rows[target]->setFocus(Qt::TabFocusReason);
        }
    };

    bool firstCategory = true;
    for (const auto category : analysis_plugin_categories()) {
        QVector<const AnalysisPluginDesc*> inCategory;
        for (const auto& plugin : analysis_plugins()) {
            if (plugin.category == category && availableIds.contains(plugin.id)) {
                inCategory.push_back(&plugin);
            }
        }
        if (inCategory.isEmpty()) {
            continue;
        }

        if (!firstCategory) {
            lay->addSpacing(8);
        }
        firstCategory = false;

        // A heading is a plain label in the layout, not a list row — so it is
        // unclickable and unfocusable without any special-casing.
        auto* heading = new QLabel(analysis_plugin_category_label(category));
        heading->setProperty("role", "section");
        heading->setContentsMargins(kTextLeft, 0, 8, 4);
        lay->addWidget(heading);

        for (const auto* plugin : inCategory) {
            auto* row = new PluginRowW(*plugin, on_click, on_focus_neighbour);
            lay->addWidget(row);
            d->rows.push_back(row);
        }
    }

    lay->addStretch(1);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);

    // Wide enough for the longest label at its normal size, narrow enough that
    // it can't crowd out the results view beside it.
    setMinimumWidth(190);
    setMaximumWidth(300);
}

AnalysisPluginRailW::~AnalysisPluginRailW() = default;

void AnalysisPluginRailW::set_current(const QString& pluginId) {
    if (d->current == pluginId) {
        return;
    }
    d->current = pluginId;
    for (auto* row : d->rows) {
        row->set_selected(row->plugin_id() == pluginId);
    }
}

QString AnalysisPluginRailW::current_plugin() const { return d->current; }

void AnalysisPluginRailW::set_run_states(const QHash<QString, PluginRunState>& states) {
    for (auto* row : d->rows) {
        row->set_run_state(states.value(row->plugin_id(), PluginRunState::Unknown));
    }
}

} // namespace mosaic
