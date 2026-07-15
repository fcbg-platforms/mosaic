
.. _program_listing_file_src_trigger_parallel_port_trigger.cpp:

Program Listing for File parallel_port_trigger.cpp
==================================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_trigger_parallel_port_trigger.cpp>` (``src/trigger/parallel_port_trigger.cpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #include "trigger/parallel_port_trigger.hpp"
   #include "utils/logger.hpp"
   #include "utils/timestamp.hpp"
   #include <QTimer>
   #include <array>
   #include <atomic>
   
   #if defined(MOSAIC_HAVE_PARALLEL_PORT) && defined(Q_OS_WIN)
   #  define WIN32_LEAN_AND_MEAN
   #  include <windows.h>
   #endif
   
   namespace mosaic {
   
   // ── InpOut32 dynamic loading ───────────────────────────────────────────────
   
   #if defined(MOSAIC_HAVE_PARALLEL_PORT) && defined(Q_OS_WIN)
   
   using Inp32Fn = short (__stdcall*)(short portAddress);
   using Out32Fn = void  (__stdcall*)(short portAddress, short data);
   
   struct InpOut32 {
       HMODULE  handle{nullptr};
       Inp32Fn  inp32{nullptr};
       Out32Fn  out32{nullptr};
   
       bool load() {
           handle = LoadLibraryA("inpout32.dll");
           if (!handle) { handle = LoadLibraryA("inpoutx64.dll"); }
           if (!handle) { return false; }
           inp32 = reinterpret_cast<Inp32Fn>(GetProcAddress(handle, "Inp32"));
           out32 = reinterpret_cast<Out32Fn>(GetProcAddress(handle, "Out32"));
           return inp32 != nullptr && out32 != nullptr;
       }
   
       void unload() {
           if (handle) { FreeLibrary(handle); handle = nullptr; }
           inp32 = nullptr;
           out32 = nullptr;
       }
   
       ~InpOut32() { unload(); }
   };
   
   #endif
   
   // ── Impl ───────────────────────────────────────────────────────────────────
   
   struct ParallelPortTrigger::Impl {
       ParallelPortConfig config;
       QTimer             timer;
       uint8_t            lastByte{0xFF};
       std::atomic<int>   eventCount{0};
       bool               active{false};
   
   #if defined(MOSAIC_HAVE_PARALLEL_PORT) && defined(Q_OS_WIN)
       InpOut32           driver;
       short              portAddr{0x378};
   #endif
   
       explicit Impl(const ParallelPortConfig& cfg) : config(cfg) {}
   
       // Read one byte from the port; returns 0 and logs on error.
       uint8_t read_port() {
   #if defined(MOSAIC_HAVE_PARALLEL_PORT) && defined(Q_OS_WIN)
           if (!driver.inp32) { return 0; }
           return static_cast<uint8_t>(driver.inp32(portAddr) & 0xFF);
   #else
           return 0;
   #endif
       }
   
       uint8_t effective_byte(uint8_t raw) const {
           return config.invertLogic ? static_cast<uint8_t>(~raw) : raw;
       }
   
       static short parse_port_address(const QString& addr) {
           bool ok{false};
           const int val = addr.toInt(&ok, 0);
           return ok ? static_cast<short>(val) : static_cast<short>(0x378);
       }
   };
   
   // ── ParallelPortTrigger ────────────────────────────────────────────────────
   
   ParallelPortTrigger::ParallelPortTrigger(const ParallelPortConfig& config,
                                             QObject* parent)
       : QObject(parent), d(std::make_unique<Impl>(config))
   {
       connect(&d->timer, &QTimer::timeout, this, [this] {
           const uint8_t raw      = d->read_port();
           const uint8_t current  = d->effective_byte(raw);
           const uint8_t prev     = d->effective_byte(d->lastByte);
           const uint8_t changed  = current ^ prev;
           d->lastByte = raw;
   
           for (int bit = 0; bit < 8; ++bit) {
               if (!(changed & (1u << bit))) { continue; }
   
               const bool rising = (current >> bit) & 1u;
               TriggerEvent ev;
               ev.timestampNs = elapsed_ns();
               ev.source      = "parallel_port";
               ev.label       = QString("D%1_%2").arg(bit).arg(rising ? "RISE" : "FALL");
               ev.value       = rising ? 1.0 : 0.0;
               d->eventCount.fetch_add(1);
               emit triggered(std::move(ev));
           }
       });
   }
   
   ParallelPortTrigger::~ParallelPortTrigger() { stop(); }
   
   bool ParallelPortTrigger::start() {
       if (!d->config.enabled) { return false; }
   
   #if defined(MOSAIC_HAVE_PARALLEL_PORT) && defined(Q_OS_WIN)
       d->portAddr = Impl::parse_port_address(d->config.portAddress);
   
       if (!d->driver.load()) {
           const QString msg = "InpOut32.dll not found. "
               "Download from http://www.highrez.co.uk/downloads/inpout32/ "
               "and place it next to mosaic.exe.";
           log_error(QString("[ParallelPort] %1").arg(msg));
           emit error_occurred(msg);
           return false;
       }
   
       // Read initial state so first poll doesn't generate spurious events.
       d->lastByte = static_cast<uint8_t>(d->driver.inp32(d->portAddr) & 0xFF);
       d->timer.setInterval(d->config.pollRateMs);
       d->timer.start();
       d->active = true;
       log_info(QString("[ParallelPort] Polling 0x%1 every %2 ms.")
                    .arg(d->config.portAddress).arg(d->config.pollRateMs));
       return true;
   #else
       log_warning("[ParallelPort] Parallel port support not compiled in.");
       emit error_occurred("Parallel port support requires Windows + InpOut32.");
       return false;
   #endif
   }
   
   void ParallelPortTrigger::stop() {
       d->timer.stop();
   
   #if defined(MOSAIC_HAVE_PARALLEL_PORT) && defined(Q_OS_WIN)
       d->driver.unload();
   #endif
   
       d->active = false;
   }
   
   bool ParallelPortTrigger::is_active()    const { return d->active; }
   int  ParallelPortTrigger::events_fired() const {
       return d->eventCount.load();
   }
   
   } // namespace mosaic
