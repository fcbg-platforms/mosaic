#pragma once
#include "core/settings.hpp"
#include <QAudioDevice>
#include <QList>
#include <QObject>
#include <memory>
#include <vector>

namespace mosaic {

/// @brief Coordinates all AudioRecorder instances for a recording session.
///
/// AudioManager is owned by Application and shared between RecordManager
/// (recording lifecycle) and AudioSettingsW (device enumeration and live
/// level display).
///
/// @par Per-microphone file naming
/// Files are written to @c \<sessionDir\>/\<basename\>_N.wav.
/// When only one microphone is configured the @c _0 suffix is omitted.
///
/// @par RMS metering
/// Each recorder emits level_rms_changed() approximately 10–20 times per second.
/// Connect to this signal from the main thread to drive a VU meter widget:
/// @code{.cpp}
/// connect(audioMgr, &AudioManager::level_rms_changed, vuMeter,
///         [vuMeter](int micIdx, float rms) {
///             if (micIdx == 0) vuMeter->set_value(rms);
///         });
/// @endcode
///
/// @see AudioRecorder, MicrophoneParameters
class AudioManager : public QObject {
    Q_OBJECT
public:
    explicit AudioManager(QObject* parent = nullptr);
    ~AudioManager() override;

    // ── Device enumeration ─────────────────────────────────────────────────

    /// @returns All available audio input devices on this system.
    [[nodiscard]] static QList<QAudioDevice> available_inputs();

    /// @returns The system default audio input device.
    [[nodiscard]] static QAudioDevice default_input();

    // ── Recording lifecycle ────────────────────────────────────────────────

    /// @brief Starts one AudioRecorder per entry in @p microphones.
    ///
    /// Stops any previous monitoring session first.
    /// Recorders that fail to start are logged but do not abort the others.
    ///
    /// @param sessionDir   Absolute path to the session folder.
    /// @param basename     File basename, e.g. @c "audio".
    /// @param microphones  Per-microphone configuration from AppSettings.
    void start(const QString&                        sessionDir,
               const QString&                        basename,
               const std::vector<MicrophoneParameters>& microphones);

    /// @brief Starts level-metering recorders without writing any files.
    ///
    /// Used to drive the live waveform display outside of a recording session.
    /// Call start() (the recording version) to replace monitoring with actual recording.
    void start_monitoring(const std::vector<MicrophoneParameters>& microphones);

    /// @brief Stops monitoring-only recorders.
    void stop_monitoring();

    /// @brief Stops all recording recorders and flushes WAV headers.
    void stop();

    /// @returns @c true while at least one recording recorder is active.
    [[nodiscard]] bool is_recording()  const;

    /// @returns @c true while monitoring-only recorders are running.
    [[nodiscard]] bool is_monitoring() const;

    /// @returns The number of recorders that were successfully started.
    [[nodiscard]] int  recorder_count() const;

signals:
    /// Emitted from the main thread (queued from the audio capture thread).
    /// @param micIndex  Zero-based index into the active microphone list.
    /// @param rms       Normalised RMS level in the range [0.0, 1.0].
    void level_rms_changed(int micIndex, float rms);

    /// Emitted when a recorder encounters a device error.
    /// @param micIndex  Zero-based index of the failing recorder.
    /// @param message   Human-readable error string.
    void recorder_error(int micIndex, QString message);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
