
.. _program_listing_file_src_trigger_lsl_inlet.hpp:

Program Listing for File lsl_inlet.hpp
======================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_trigger_lsl_inlet.hpp>` (``src\trigger\lsl_inlet.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include "core/settings.hpp"
   #include "trigger/trigger_types.hpp"
   #include <QObject>
   #include <memory>
   
   namespace mosaic {
   
   // Receives string markers from a Lab Streaming Layer inlet and fires
   // TriggerEvents into TriggerManager.
   //
   // Each LslInlet runs its own polling thread so that slow streams do not
   // affect the main thread. The received() signal is emitted via a queued
   // connection so it is safe to connect to main-thread slots.
   //
   // Requires MOSAIC_HAVE_LSL at compile time; constructed but immediately
   // inactive otherwise.
   
   class LslInlet : public QObject {
       Q_OBJECT
   public:
       explicit LslInlet(const LslInletConfig& config, QObject* parent = nullptr);
       ~LslInlet() override;
   
       // Resolves the stream and starts the polling thread.
       // Returns false if LSL is not available or the stream cannot be found
       // within the timeout (seconds).
       [[nodiscard]] bool connect(double resolveTimeoutSec = 5.0);
       void               disconnect();
   
       [[nodiscard]] bool    is_connected() const;
       [[nodiscard]] int64_t samples_received() const;
   
   signals:
       void received(mosaic::TriggerEvent event);
       void connection_lost(QString streamName);
   
   private:
       struct Impl;
       std::unique_ptr<Impl> d;
   };
   
   } // namespace mosaic
