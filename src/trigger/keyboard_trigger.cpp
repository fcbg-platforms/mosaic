#include "trigger/keyboard_trigger.hpp"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QKeySequenceEdit>

#include "utils/timestamp.hpp"

namespace mosaic {

KeyboardTrigger::KeyboardTrigger(KeyTriggerConfig& config, QObject* parent)
    : QObject(parent), m_config(config) {
    reload_key_sequence();
}

KeyboardTrigger::~KeyboardTrigger() { set_active(false); }

void KeyboardTrigger::set_active(bool active) {
    if (m_active == active) return;
    m_active = active;
    if (active)
        qApp->installEventFilter(this);
    else
        qApp->removeEventFilter(this);
}

void KeyboardTrigger::reload_key_sequence() {
    m_keySeq = QKeySequence(m_config.keySeq, QKeySequence::PortableText);
}

void KeyboardTrigger::reset_count() {
    m_fireCount = 0;
    emit count_changed(0);
}

namespace {
// Real-world logs from this same key-binding feature showed 9 near-
// simultaneous KeyPress events for a single physical press, spaced
// irregularly (tens to hundreds of ms apart) — too irregular for classic
// OS auto-repeat (which is a fixed-rate stream) and NOT flagged by
// QKeyEvent::isAutoRepeat() at all, so that check alone (tried first)
// didn't fix it. Root cause wasn't pinned down further (no way to attach
// a debugger to the exact hardware/driver that produced it), so the fix
// below is state-based rather than event-flag-based: explicitly track
// "is this key currently down" and refuse a second fire until a matching
// KeyRelease is seen, however many duplicate KeyPress events arrive in
// between and whatever produced them.
constexpr int64_t k_stale_key_down_ns = 3'000'000'000LL; // 3s
} // namespace

bool KeyboardTrigger::eventFilter(QObject* /*obj*/, QEvent* event) {
    const bool isPress   = event->type() == QEvent::KeyPress;
    const bool isRelease = event->type() == QEvent::KeyRelease;
    if (!isPress && !isRelease) return false;
    if (!m_config.enabled || m_keySeq.isEmpty()) return false;

    // Don't fire while the user is recording a key binding.
    if (auto* focused = QApplication::focusWidget())
        if (focused->inherits("QKeySequenceEdit")) return false;

    auto* ke = static_cast<QKeyEvent*>(event);
    const QKeySequence pressed(ke->key() | static_cast<int>(ke->modifiers()));
    if (pressed != m_keySeq) return false;

    if (isRelease) {
        m_keyDown = false;
        return false;
    }

    // KeyPress. If we still think the key is down from an earlier press,
    // this is a duplicate — the release just hasn't been seen yet — unless
    // it's been so long that the release was very likely lost entirely
    // (e.g. focus moved to another application mid-press), in which case
    // treat this as a fresh press rather than staying stuck forever.
    const int64_t nowNs = elapsed_ns();
    if (m_keyDown && (nowNs - m_keyDownAtNs) < k_stale_key_down_ns) {
        return false;
    }

    m_keyDown     = true;
    m_keyDownAtNs = nowNs;

    ++m_fireCount;
    emit count_changed(m_fireCount);

    TriggerEvent ev;
    ev.timestampNs = nowNs;
    ev.source      = "keyboard";
    ev.label       = m_config.name;
    ev.code        = m_config.code;
    ev.value       = 0.0;
    emit triggered(ev);

    return false; // never consume — other widgets should still handle the key
}

} // namespace mosaic
