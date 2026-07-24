
.. _program_listing_file_src_trigger_parallel_port_trigger.hpp:

Program Listing for File parallel_port_trigger.hpp
==================================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_trigger_parallel_port_trigger.hpp>` (``src\trigger\parallel_port_trigger.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include "core/settings.hpp"
   #include "trigger/trigger_types.hpp"
   #include <QObject>
   #include <memory>
   
   namespace mosaic {
   
   // Monitors a parallel port data register (LPT) for bit-level changes and
   // fires TriggerEvents on rising and falling edges.
   //
   // Implementation:
   //   Windows + MOSAIC_HAVE_PARALLEL_PORT: dynamically loads InpOut32.dll;
   //     polls at the rate specified in ParallelPortConfig.pollRateMs.
   //   All other configurations: stub (always inactive, logs a warning).
   //
   // Each of the 8 data bits generates its own TriggerEvent with
   //   source = "parallel_port"
   //   label  = "D<bit>_<RISE|FALL>"   e.g. "D3_RISE"
   //   value  = 1.0 (rising) or 0.0 (falling)
   //
   // Bit polarity: invertLogic = false → active-high (default).
   //               invertLogic = true  → active-low (common in TTL circuits).
   
   class ParallelPortTrigger : public QObject {
       Q_OBJECT
   public:
       explicit ParallelPortTrigger(const ParallelPortConfig& config,
                                     QObject* parent = nullptr);
       ~ParallelPortTrigger() override;
   
       // Loads InpOut32 and starts the polling timer.
       [[nodiscard]] bool start();
       void               stop();
   
       [[nodiscard]] bool is_active()       const;
       [[nodiscard]] int  events_fired()    const;
   
   signals:
       void triggered(mosaic::TriggerEvent event);
       void error_occurred(QString message);
   
   private:
       struct Impl;
       std::unique_ptr<Impl> d;
   };
   
   } // namespace mosaic
