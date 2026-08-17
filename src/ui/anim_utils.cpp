#include "ui/anim_utils.hpp"

#include <QAbstractAnimation>
#include <QEasingCurve>
#include <QHash>
#include <QPoint>
#include <QPointer>
#include <QSequentialAnimationGroup>
#include <algorithm>

namespace mosaic::anim {

QGraphicsOpacityEffect* ensure_opacity_effect(QWidget* w) {
    auto* effect = qobject_cast<QGraphicsOpacityEffect*>(w->graphicsEffect());
    if (!effect) {
        effect = new QGraphicsOpacityEffect(w);
        w->setGraphicsEffect(effect);
    }
    return effect;
}

void fade_in_widget(QWidget* w, int durationMs) {
    auto* effect = ensure_opacity_effect(w);
    auto* anim   = new QPropertyAnimation(effect, "opacity", w);
    anim->setDuration(durationMs);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void fade_out_widget(QWidget* w, int durationMs, std::function<void()> onFinished) {
    auto* effect = ensure_opacity_effect(w);
    auto* anim   = new QPropertyAnimation(effect, "opacity", w);
    anim->setDuration(durationMs);
    anim->setStartValue(effect->opacity());
    anim->setEndValue(0.0);
    QObject::connect(anim, &QAbstractAnimation::finished, w, [w, onFinished] {
        if (onFinished) {
            onFinished();
        } else {
            w->hide();
        }
    });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

namespace {

// Tracks any currently in-flight shake_widget() animation per widget, so a
// second shake requested before the first settles (e.g. two failed attempts
// in quick succession) resumes from the widget's TRUE rest position instead
// of wherever it happens to be mid-shake, and cancels the stale group rather
// than letting two animations race on the same `pos` property.
struct ShakeState {
    QPoint basePos;
    QAbstractAnimation* group = nullptr;
};
QHash<QWidget*, ShakeState>& shake_states() {
    static QHash<QWidget*, ShakeState> states;
    return states;
}

} // namespace

void shake_widget(QWidget* w) {
    if (!w) {
        return;
    }
    auto& states              = shake_states();
    const auto it             = states.find(w);
    const bool alreadyShaking = (it != states.end());
    const QPoint base         = alreadyShaking ? it->basePos : w->pos();
    if (alreadyShaking) {
        it->group->stop();
    }

    auto* group         = new QSequentialAnimationGroup(w);
    const int offsets[] = {8, -8, 6, -6, 3, -3, 0};
    QPoint prev         = base;
    for (int off : offsets) {
        const QPoint target = base + QPoint(off, 0);
        auto* anim          = new QPropertyAnimation(w, "pos");
        anim->setDuration(45);
        anim->setEasingCurve(QEasingCurve::InOutQuad);
        anim->setStartValue(prev);
        anim->setEndValue(target);
        group->addAnimation(anim);
        prev = target;
    }
    states[w] = ShakeState{base, group};
    // finished() only fires on natural completion, never on stop() (see the
    // re-entrant path above) — so a superseded group's own handler simply
    // never runs, which is correct: the map entry it would have cleared was
    // already overwritten synchronously by the new shake before this point.
    QObject::connect(group, &QAbstractAnimation::finished, w, [w, group] {
        auto& s          = shake_states();
        const auto found = s.find(w);
        if (found != s.end() && found->group == group) {
            s.remove(w);
        }
    });
    // Guards against w being destroyed mid-shake (e.g. dialog teardown)
    // leaving a stale entry keyed by a dangling pointer that a future,
    // unrelated widget allocated at the same address could wrongly match.
    QObject::connect(w, &QObject::destroyed, [w] { shake_states().remove(w); });
    group->start(QAbstractAnimation::DeleteWhenStopped);
}

QColor lerp_color(const QColor& a, const QColor& b, qreal t) {
    t = std::clamp(t, 0.0, 1.0);
    return QColor(static_cast<int>(a.red() + (b.red() - a.red()) * t),
                  static_cast<int>(a.green() + (b.green() - a.green()) * t),
                  static_cast<int>(a.blue() + (b.blue() - a.blue()) * t),
                  static_cast<int>(a.alpha() + (b.alpha() - a.alpha()) * t));
}

void restart_hover_anim(QVariantAnimation* anim, qreal currentT, qreal target) {
    anim->stop();
    anim->setStartValue(currentT);
    anim->setEndValue(target);
    anim->start();
}

namespace {

// Tracks any currently in-flight animate_collapse() animation per body
// widget, so a fast double-click starting a second collapse/expand before
// the first settles cancels the stale animation instead of letting two
// animations race on the same maximumHeight property.
struct CollapseState {
    QPointer<QPropertyAnimation> anim;
};
QHash<QWidget*, CollapseState>& collapse_states() {
    static QHash<QWidget*, CollapseState> states;
    return states;
}

} // namespace

QPropertyAnimation* animate_collapse(QWidget* body, bool expand, int durationMs,
                                     std::function<void()> onFinished) {
    auto& states = collapse_states();
    auto it      = states.find(body);
    if (it == states.end()) {
        QObject::connect(body, &QObject::destroyed, [body] { collapse_states().remove(body); });
        it = states.insert(body, CollapseState{});
    } else if (it->anim) {
        it->anim->stop();
    }

    auto* anim = new QPropertyAnimation(body, "maximumHeight", body);
    anim->setDuration(durationMs);
    anim->setEasingCurve(QEasingCurve::InOutCubic);

    if (expand) {
        body->show();
        body->setMaximumHeight(0);
        anim->setStartValue(0);
        anim->setEndValue(body->sizeHint().height());
        QObject::connect(anim, &QAbstractAnimation::finished, body, [body, onFinished] {
            body->setMaximumHeight(QWIDGETSIZE_MAX);
            if (onFinished) {
                onFinished();
            }
        });
    } else {
        anim->setStartValue(body->height());
        anim->setEndValue(0);
        QObject::connect(anim, &QAbstractAnimation::finished, body, [body, onFinished] {
            body->hide();
            if (onFinished) {
                onFinished();
            }
        });
    }

    it->anim = anim;
    anim->start(QAbstractAnimation::DeleteWhenStopped);
    return anim;
}

namespace {

// Re-entrancy-guard state for crossfade_stacked_widget(), keyed per stack —
// see the header doc comment for why this can't live in each caller's own
// Impl struct. targetIndex is lazily seeded from stack->currentIndex() the
// first time a given stack is seen.
struct StackTransitionState {
    int targetIndex = -1;
    QPointer<QPropertyAnimation> anim;
};
QHash<QStackedWidget*, StackTransitionState>& stack_transition_states() {
    static QHash<QStackedWidget*, StackTransitionState> states;
    return states;
}

} // namespace

void crossfade_stacked_widget(QStackedWidget* stack, int newIndex, int durationMs,
                              std::function<void()> onComplete) {
    auto& states = stack_transition_states();
    auto it      = states.find(stack);
    if (it == states.end()) {
        QObject::connect(stack, &QObject::destroyed,
                         [stack] { stack_transition_states().remove(stack); });
        it = states.insert(stack, StackTransitionState{stack->currentIndex(), nullptr});
    }

    // Already showing (or already mid-transition toward) this page — a
    // repeated call (e.g. clicking the same nav control twice) is a no-op.
    if (it->targetIndex == newIndex) {
        return;
    }
    it->targetIndex = newIndex;

    // Cancel whichever phase (out or in) of a still-in-flight transition is
    // currently running — without this, fast repeated switching can start a
    // second crossfade before the first one reaches setCurrentIndex(),
    // leaving two animations racing on the same opacity effect.
    if (it->anim) {
        it->anim->stop();
    }

    QWidget* currentPage = stack->currentWidget();
    QWidget* targetPage  = stack->widget(newIndex);
    if (!targetPage || currentPage == targetPage) {
        if (targetPage) {
            ensure_opacity_effect(targetPage)->setOpacity(1.0);
        }
        stack->setCurrentIndex(newIndex);
        if (targetPage && onComplete) {
            onComplete();
        }
        return;
    }

    auto* outEffect = ensure_opacity_effect(currentPage);
    auto* outAnim   = new QPropertyAnimation(outEffect, "opacity", currentPage);
    outAnim->setDuration(durationMs);
    outAnim->setEasingCurve(QEasingCurve::InOutCubic);
    outAnim->setStartValue(outEffect->opacity());
    outAnim->setEndValue(0.0);
    it->anim = outAnim; // `it` still valid: no intervening hash mutation since find/insert above

    QObject::connect(outAnim, &QAbstractAnimation::finished, stack,
                     [stack, newIndex, targetPage, durationMs, onComplete] {
                         stack->setCurrentIndex(newIndex);
                         if (onComplete) {
                             onComplete();
                         }

                         auto* inEffect = ensure_opacity_effect(targetPage);
                         inEffect->setOpacity(0.0);
                         auto* inAnim = new QPropertyAnimation(inEffect, "opacity", targetPage);
                         inAnim->setDuration(durationMs);
                         inAnim->setEasingCurve(QEasingCurve::InOutCubic);
                         inAnim->setStartValue(0.0);
                         inAnim->setEndValue(1.0);
                         // Re-fetch rather than capturing a reference/iterator from the
                         // outer scope: this lambda runs later, after other
                         // crossfade_stacked_widget() calls (for other stacks, or a
                         // re-entrant call for this one) may have inserted into / rehashed
                         // the shared side-table, which would invalidate anything captured
                         // from before.
                         stack_transition_states()[stack].anim = inAnim;
                         inAnim->start(QAbstractAnimation::DeleteWhenStopped);
                     });

    outAnim->start(QAbstractAnimation::DeleteWhenStopped);
}

} // namespace mosaic::anim
