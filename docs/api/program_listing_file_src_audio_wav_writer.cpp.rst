
.. _program_listing_file_src_audio_wav_writer.cpp:

Program Listing for File wav_writer.cpp
=======================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_audio_wav_writer.cpp>` (``src\audio\wav_writer.cpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #include "audio/wav_writer.hpp"
   #include "utils/logger.hpp"
   #include <QFile>
   #include <QMutex>
   #include <QMutexLocker>
   #include <cstdint>
   
   namespace mosaic {
   
   // ── Helpers ────────────────────────────────────────────────────────────────
   
   static void write_le16(QFile& f, uint16_t v) {
       const uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
       f.write(reinterpret_cast<const char*>(b), 2);
   }
   
   static void write_le32(QFile& f, uint32_t v) {
       const uint8_t b[4] = {
           uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)
       };
       f.write(reinterpret_cast<const char*>(b), 4);
   }
   
   static void poke_le32(QFile& f, qint64 pos, uint32_t v) {
       const qint64 cur = f.pos();
       f.seek(pos);
       write_le32(f, v);
       f.seek(cur);
   }
   
   // ── Impl ───────────────────────────────────────────────────────────────────
   
   struct WavWriter::Impl {
       mutable QMutex mutex;
       QFile   file;
       bool    open{false};
       bool    error{false};
       int     sampleRate{44100};
       int     channels{2};
       int     bitsPerSample{16};
       int64_t bytesWritten{0};
   };
   
   // ── WavWriter ──────────────────────────────────────────────────────────────
   
   WavWriter::WavWriter()  : d(std::make_unique<Impl>()) {}
   WavWriter::~WavWriter() { close(); }
   
   bool WavWriter::open(const QString& path, int sampleRate, int channels, int bitsPerSample) {
       QMutexLocker lock(&d->mutex);
   
       d->file.setFileName(path);
       if (!d->file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
           log_error(QString("[WavWriter] Cannot open: %1 — %2")
                         .arg(path, d->file.errorString()));
           return false;
       }
   
       d->sampleRate    = sampleRate;
       d->channels      = channels;
       d->bitsPerSample = bitsPerSample;
       d->bytesWritten  = 0;
       d->error         = false;
   
       // ── Write RIFF/WAV header (sizes are 0; fixed on close) ───────────────
       const uint32_t byteRate   = uint32_t(sampleRate * channels * bitsPerSample / 8);
       const uint16_t blockAlign = uint16_t(channels * bitsPerSample / 8);
   
       d->file.write("RIFF", 4);
       write_le32(d->file, 0);             // ← riffSize placeholder  (offset 4)
       d->file.write("WAVE", 4);
       d->file.write("fmt ", 4);
       write_le32(d->file, 16);            // fmt chunk size (always 16 for PCM)
       write_le16(d->file, 1);             // audio format = PCM
       write_le16(d->file, uint16_t(channels));
       write_le32(d->file, uint32_t(sampleRate));
       write_le32(d->file, byteRate);
       write_le16(d->file, blockAlign);
       write_le16(d->file, uint16_t(bitsPerSample));
       d->file.write("data", 4);
       write_le32(d->file, 0);             // ← dataSize placeholder   (offset 40)
   
       d->open = true;
       log_info(QString("[WavWriter] Opened %1  (%2 Hz, %3 ch, %4 bit)")
                    .arg(path).arg(sampleRate).arg(channels).arg(bitsPerSample));
       return true;
   }
   
   bool WavWriter::write(const char* data, qint64 bytes) {
       QMutexLocker lock(&d->mutex);
       if (!d->open || d->error) return false;
   
       const qint64 written = d->file.write(data, bytes);
       if (written != bytes) {
           log_error(QString("[WavWriter] Disk write error: %1").arg(d->file.errorString()));
           d->error = true;
           return false;
       }
       d->bytesWritten += written;
       return true;
   }
   
   void WavWriter::close() {
       QMutexLocker lock(&d->mutex);
       if (!d->open) return;
   
       // Fix up chunk sizes in the header
       const uint32_t dataSize = static_cast<uint32_t>(
           std::min(d->bytesWritten, int64_t(0xFFFF'FFFF)));
       const uint32_t riffSize = 36 + dataSize;
   
       poke_le32(d->file, 4,  riffSize);
       poke_le32(d->file, 40, dataSize);
   
       d->file.flush();
       d->file.close();
       d->open = false;
   
       log_info(QString("[WavWriter] Closed — %1 bytes, %.2f s")
                    .arg(d->bytesWritten)
                    .arg(double(d->bytesWritten) /
                         (d->sampleRate * d->channels * d->bitsPerSample / 8)));
   }
   
   bool    WavWriter::is_open()       const { QMutexLocker l(&d->mutex); return d->open; }
   int64_t WavWriter::bytes_written() const { QMutexLocker l(&d->mutex); return d->bytesWritten; }
   
   double WavWriter::duration_sec() const {
       QMutexLocker l(&d->mutex);
       const int bytesPerSec = d->sampleRate * d->channels * d->bitsPerSample / 8;
       return bytesPerSec > 0 ? double(d->bytesWritten) / bytesPerSec : 0.0;
   }
   
   } // namespace mosaic
