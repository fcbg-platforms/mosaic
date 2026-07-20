
.. _program_listing_file_src_trigger_keyboard_trigger.hpp:

Program Listing for File keyboard_trigger.hpp
=============================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_trigger_keyboard_trigger.hpp>` (``src\trigger\keyboard_trigger.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include "core/settings.hpp"
   #include "trigger/trigger_types.hpp"
   #include <QKeySequence>
   #include <QObject>
   
   namespace mosaic {
   
   // One keyboard trigger. Installed as a QCoreApplication event filter so it
   // catches key events from any focused widget inside the app.
   // Does NOT fire when a QKeySequenceEdit is focused (prevents double-triggering
   // while the user is recording a new key binding).
   
   class KeyboardTrigger : public QObject {
       Q_OBJECT
   public:
       explicit KeyboardTrigger(KeyTriggerConfig& config, QObject* parent = nullptr);
       ~KeyboardTrigger() override;
   
       void set_active(bool active);
       [[nodiscard]] bool is_active() const { return m_active; }
   
       // Call after the config's keySeq string changes to re-parse it.
       void reload_key_sequence();
   
       [[nodiscard]] int fire_count() const { return m_fireCount; }
       void reset_count();
   
       bool eventFilter(QObject* obj, QEvent* event) override;
   
   signals:
       void triggered(mosaic::TriggerEvent event);
       void count_changed(int count);
   
   private:
       KeyTriggerConfig& m_config;
       QKeySequence      m_keySeq;
       bool              m_active{false};
       int               m_fireCount{0};
   };
   
   } // namespace mosaic
