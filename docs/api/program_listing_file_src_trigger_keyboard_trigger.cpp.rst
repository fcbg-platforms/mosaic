
.. _program_listing_file_src_trigger_keyboard_trigger.cpp:

Program Listing for File keyboard_trigger.cpp
=============================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_trigger_keyboard_trigger.cpp>` (``src\trigger\keyboard_trigger.cpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #include "trigger/keyboard_trigger.hpp"
   #include "utils/timestamp.hpp"
   #include <QApplication>
   #include <QEvent>
   #include <QKeyEvent>
   #include <QKeySequenceEdit>
   
   namespace mosaic {
   
   KeyboardTrigger::KeyboardTrigger(KeyTriggerConfig& config, QObject* parent)
       : QObject(parent), m_config(config)
   {
       reload_key_sequence();
   }
   
   KeyboardTrigger::~KeyboardTrigger() {
       set_active(false);
   }
   
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
   
   bool KeyboardTrigger::eventFilter(QObject* /*obj*/, QEvent* event) {
       if (event->type() != QEvent::KeyPress) return false;
       if (!m_config.enabled || m_keySeq.isEmpty()) return false;
   
       // Don't fire while the user is recording a key binding.
       if (auto* focused = QApplication::focusWidget())
           if (focused->inherits("QKeySequenceEdit")) return false;
   
       auto* ke = static_cast<QKeyEvent*>(event);
       const QKeySequence pressed(ke->key() | static_cast<int>(ke->modifiers()));
   
       if (pressed != m_keySeq) return false;
   
       ++m_fireCount;
       emit count_changed(m_fireCount);
   
       TriggerEvent ev;
       ev.timestampNs = elapsed_ns();
       ev.source      = "keyboard";
       ev.label       = m_config.name;
       ev.value       = 0.0;
       emit triggered(ev);
   
       return false;  // never consume — other widgets should still handle the key
   }
   
   } // namespace mosaic
