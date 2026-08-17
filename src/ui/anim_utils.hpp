#pragma once
#include <QColor>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QStackedWidget>
#include <QVariantAnimation>
#include <QWidget>
#include <functional>

// Small, shared animation/paint helpers reused across the UI so each widget
// that wants a fade/crossfade/collapse/shake doesn't re-copy its own private
// version. Originally written for LoginDialog and CameraCardW; promoted here
// once a third+fourth consumer needed the same recipes.

namespace mosaic::anim {

// Lazily installs (or reuses) a QGraphicsOpacityEffect on `w` — every fade/
// crossfade below shares this instead of each caller keeping its own
// persisted effect pointer; leaving the effect installed at opacity 1.0 is
// harmless.
QGraphicsOpacityEffect* ensure_opacity_effect(QWidget* w);

// Opacity 0→1 over `durationMs`.
void fade_in_widget(QWidget* w, int durationMs = 150);

// Opacity 1→0 over `durationMs`, then calls `onFinished` (defaults to
// hiding `w`, the common case). Kept distinct from fade_in_widget rather
// than a single fade_widget(bool) — callers almost always want a different
// completion action for each direction (hide vs. nothing).
void fade_out_widget(QWidget* w, int durationMs = 150, std::function<void()> onFinished = {});

// Short, decaying horizontal shake — the standard "invalid input" cue.
// Animates the widget's own `pos` property relative to wherever it
// currently sits under its layout, and always ends exactly back at that
// position. Safe to call again on a still-shaking widget (resumes from its
// true rest position rather than letting two animations race).
void shake_widget(QWidget* w);

// Linear per-channel RGBA interpolation, t clamped to [0, 1].
[[nodiscard]] QColor lerp_color(const QColor& a, const QColor& b, qreal t);

// Restarts a 0..1 hover-intensity animation toward `target` from wherever
// it currently is, so a fast in-out-in doesn't visually jump. Works for any
// QVariantAnimation, whether owned by a QWidget (e.g. AvatarChip) or by a
// non-widget QObject such as a QStyledItemDelegate.
void restart_hover_anim(QVariantAnimation* anim, qreal currentT, qreal target);

// Generalizes CameraCardW::toggle_expanded()'s recipe (also copy-pasted in
// KeyboardCardW/MicrophoneCardW/SerialCardW): animates `body`'s
// maximumHeight between 0 and its sizeHint, showing/hiding it at the right
// end of the animation. The caller still owns its own `bool expanded` flag
// and any arrow-glyph swap — this owns only the body widget's animation.
//
// Unlike the four existing private copies, this is safe against a fast
// double-click starting two animations on the same maximumHeight property:
// re-entrancy state is tracked internally in a side-table keyed by `body`,
// so a second call while the first is still running stops the stale
// animation before starting the new one.
QPropertyAnimation* animate_collapse(QWidget* body, bool expand, int durationMs = 180,
                                     std::function<void()> onFinished = {});

// Generalizes LoginDialog::crossfade_to_page() into a reusable sequential
// fade-out → setCurrentIndex → fade-in for any QStackedWidget (not just a
// 2-page case) — QStackedWidget only ever paints one page at a time, so a
// true overlapping crossfade isn't possible without restructuring away from
// it; this reads as a smooth transition rather than a hard cut.
//
// Re-entrancy-guard state (in-flight animation + target index) lives in an
// internal side-table keyed by `stack`, not in the caller's own Impl struct
// — a free function has no access to a caller's private members, and
// requiring every caller to hand-roll its own guard would defeat the point
// of sharing this logic. A stack's first-use target index is lazily seeded
// from `stack->currentIndex()`, so a caller whose stack doesn't start on
// page 0 is still handled correctly.
//
// `onComplete` fires once `newIndex` becomes current (right when
// setCurrentIndex() runs, at the start of the fade-in phase — not after the
// fade-in finishes).
void crossfade_stacked_widget(QStackedWidget* stack, int newIndex, int durationMs = 130,
                              std::function<void()> onComplete = {});

} // namespace mosaic::anim
