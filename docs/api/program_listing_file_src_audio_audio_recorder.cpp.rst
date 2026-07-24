
.. _program_listing_file_src_audio_audio_recorder.cpp:

Program Listing for File audio_recorder.cpp
===========================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_audio_audio_recorder.cpp>` (``src\audio\audio_recorder.cpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #include "audio/audio_recorder.hpp"
   #include "utils/logger.hpp"
   #include <QAudioDevice>
   #include <QMediaDevices>
   #include <cmath>
   #include <cstdint>
   
   namespace mosaic {
   
   // ── Device lookup ──────────────────────────────────────────────────────────
   
   static QAudioDevice find_device(const QString& deviceId) {
       if (!deviceId.isEmpty()) {
           const QByteArray target = deviceId.toLatin1();
           for (const QAudioDevice& dev : QMediaDevices::audioInputs()) {
               if (dev.id() == target)
                   return dev;
           }
           log_warning(QString("[AudioRecorder] Device '%1' not found — using default.")
                           .arg(deviceId));
       }
       return QMediaDevices::defaultAudioInput();
   }
   
   // ── AudioRecorder ──────────────────────────────────────────────────────────
   
   AudioRecorder::AudioRecorder(const MicrophoneParameters& params, QObject* parent)
       : QObject(parent), m_params(params) {}
   
   AudioRecorder::~AudioRecorder() { stop(); }
   
   bool AudioRecorder::start(const QString& filePath) {
       m_monitorOnly = filePath.isEmpty();
   
       // 1. Open WAV file first — fail early before touching hardware.
       //    In monitor-only mode (empty filePath), skip file creation.
       if (!m_monitorOnly) {
           if (!m_writer.open(filePath, m_params.sampleRate, m_params.channels, 16)) {
               const QString msg = QString("Cannot create WAV file: %1").arg(filePath);
               log_error(msg);
               emit error_occurred(msg);
               return false;
           }
       }
   
       // 2. Find device and build format.
       const QAudioDevice device = find_device(m_params.deviceId);
       if (device.isNull()) {
           const QString msg = "No audio input device available.";
           log_error(msg);
           emit error_occurred(msg);
           m_writer.close();
           return false;
       }
   
       QAudioFormat fmt;
       fmt.setSampleRate(m_params.sampleRate);
       fmt.setChannelCount(m_params.channels);
       fmt.setSampleFormat(QAudioFormat::Int16);
   
       // Fall back to nearest supported format if needed.
       if (!device.isFormatSupported(fmt)) {
           fmt = device.preferredFormat();
           log_warning(QString("[AudioRecorder] Requested format not supported by '%1'. "
                                "Falling back to %2 Hz, %3 ch.")
                           .arg(device.description())
                           .arg(fmt.sampleRate())
                           .arg(fmt.channelCount()));
       }
   
       // 3. Create and start QAudioSource.
       m_source = std::make_unique<QAudioSource>(device, fmt, this);
       m_source->setBufferSize(
           fmt.sampleRate() * fmt.channelCount() * 2 / 10);  // ~100 ms buffer
   
       m_ioDevice = m_source->start();  // pull mode
       if (!m_ioDevice || m_source->error() != QAudio::NoError) {
           const QString msg = QString("Failed to open audio input '%1'.")
                                   .arg(device.description());
           log_error(msg);
           emit error_occurred(msg);
           if (!m_monitorOnly) m_writer.close();
           m_source.reset();
           return false;
       }
   
       connect(m_ioDevice, &QIODevice::readyRead,
               this, &AudioRecorder::on_data_ready);
   
       log_info(QString("[AudioRecorder] Started: '%1' → %2")
                    .arg(device.description(),
                         m_monitorOnly ? "(monitor only)" : filePath));
       return true;
   }
   
   void AudioRecorder::stop() {
       if (m_source) {
           m_source->stop();
           m_source.reset();
       }
       m_ioDevice = nullptr;
       m_writer.close();
       m_level.store(0.0f, std::memory_order_relaxed);
   }
   
   // ── Audio data ─────────────────────────────────────────────────────────────
   
   void AudioRecorder::on_data_ready() {
       if (!m_ioDevice) return;
       const QByteArray data = m_ioDevice->readAll();
       if (data.isEmpty()) return;
   
       if (!m_monitorOnly)
           m_writer.write(data.constData(), data.size());
   
       const float rms = compute_rms(data.constData(), data.size());
       m_level.store(rms, std::memory_order_relaxed);
       emit level_rms_changed(rms);
   }
   
   // RMS of 16-bit signed PCM samples, normalised to [0, 1].
   float AudioRecorder::compute_rms(const char* data, qint64 bytes) {
       const int numSamples = static_cast<int>(bytes / 2);
       if (numSamples == 0) return 0.0f;
   
       const auto* s = reinterpret_cast<const int16_t*>(data);
       double sum = 0.0;
       for (int i = 0; i < numSamples; ++i) {
           const double v = s[i] / 32768.0;
           sum += v * v;
       }
       return static_cast<float>(std::sqrt(sum / numSamples));
   }
   
   // ── Accessors ──────────────────────────────────────────────────────────────
   
   bool   AudioRecorder::is_recording() const {
       return m_monitorOnly ? (m_source != nullptr) : m_writer.is_open();
   }
   float  AudioRecorder::level_rms()    const { return m_level.load(std::memory_order_relaxed); }
   double AudioRecorder::duration_sec() const { return m_writer.duration_sec(); }
   
   } // namespace mosaic
