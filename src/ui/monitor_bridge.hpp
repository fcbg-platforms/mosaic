#pragma once
#include <QImage>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include "core/settings.hpp"
#include "record/record_manager.hpp"
#include "video/video_feed_provider.hpp"

namespace mosaic {

// QObject registered as QML context property "backend".
// Exposes RecordManager state and live camera frames to QML.

class MonitorBridge : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool recording READ isRecording NOTIFY recordingChanged)
    Q_PROPERTY(int elapsedMs READ elapsedMs NOTIFY elapsedMsChanged)
    Q_PROPERTY(int cameraCount READ cameraCount NOTIFY cameraCountChanged)
    Q_PROPERTY(QString sessionPath READ sessionPath NOTIFY sessionPathChanged)
    // Global counter — increments whenever any camera delivers a frame (kept for compat).
    Q_PROPERTY(int frameGen READ frameGen NOTIFY frameGenChanged)
    // Per-camera counters: frameGens[i] increments only when camera i gets a new frame,
    // so each QML slot only reloads when its own camera produces new data.
    Q_PROPERTY(QVariantList frameGens READ frameGens NOTIFY frameGensChanged)
    // Seconds left before a pending recording actually starts; 0 when idle.
    Q_PROPERTY(int countdownSeconds READ countdownSeconds NOTIFY countdownSecondsChanged)
    // True from the moment Record is clicked until the recording is actually
    // running (or the attempt is cancelled/fails). Deliberately outlives
    // countdownSeconds, which drops to 0 just *before* RecordManager::start()
    // is called: start() can block the GUI thread for a few hundred ms (it
    // creates folders, opens encoders, and waits on the camera readiness
    // barrier), and without this the previews would be free to flash back on
    // across that gap — at exactly the instant the feature exists to keep the
    // screen calm.
    Q_PROPERTY(bool startPending READ startPending NOTIFY startPendingChanged)
    // Mirror of RecordSettings::hidePreviewsWhileRecording, so QML can react
    // to the setting without reaching into AppSettings itself.
    Q_PROPERTY(bool hidePreviews READ hidePreviews NOTIFY hidePreviewsChanged)

   public:
    explicit MonitorBridge(RecordManager* recordMgr, const VideoSettings& videoSettings,
                           const RecordSettings& recordSettings, QObject* parent = nullptr);

    // Q_PROPERTY readers
    [[nodiscard]] bool isRecording() const;
    [[nodiscard]] int elapsedMs() const;
    [[nodiscard]] int cameraCount() const;
    [[nodiscard]] QString sessionPath() const;
    [[nodiscard]] int frameGen() const;
    [[nodiscard]] QVariantList frameGens() const;
    [[nodiscard]] int countdownSeconds() const;
    [[nodiscard]] bool startPending() const;
    [[nodiscard]] bool hidePreviews() const;

    // Called by VideoSettingsW when cameras are added/removed
    void set_camera_count(int count);

    // Called from MainWindow after the QML engine is set up
    void set_feed_provider(VideoFeedProvider* provider);

    // Re-reads the RecordSettings fields this bridge mirrors into QML.
    // Connected to RecordSettingsW::settings_changed so toggling "hide
    // previews" in the settings tab reaches the live monitor immediately,
    // rather than only after the next app start.
    void refresh_record_settings();

    // Aborts a pending start countdown, if one is running; a no-op otherwise.
    // Public because the countdown deliberately lives here rather than in
    // RecordManager — it's an affordance for a human clicking Record, and a
    // trigger-driven start should stay immediate — but a StopRecording
    // *trigger* firing inside the countdown window must still be able to
    // abort it, and that handler (Application's action_requested lambda)
    // only sees RecordManager, for which is_recording() is still false.
    // Reached from there via MainWindow::cancel_pending_recording_start().
    void cancel_countdown();

   public slots:
    // Arms the start countdown (RecordSettings::startDelaySec) and starts
    // recording when it reaches zero. Starts immediately if the delay is 0.
    // A no-op while already recording or already counting down, so a second
    // click (or Ctrl+R) can't double-arm it.
    Q_INVOKABLE void startRecording();
    // Cancels a pending countdown if one is running, otherwise stops the
    // recording. One control, one meaning: "make it stop".
    Q_INVOKABLE void stopRecording();

    // Connected to VideoManager::frame_preview (already on main thread via queued)
    void on_frame_preview(int cameraIndex, QImage frame);

   signals:
    void recordingChanged();
    void elapsedMsChanged();
    void cameraCountChanged();
    void sessionPathChanged();
    void frameGenChanged();
    void frameGensChanged();
    void countdownSecondsChanged();
    void startPendingChanged();
    void hidePreviewsChanged();

   private:
    // Clears the countdown (notifying QML first, so a failed start can never
    // strand the UI mid-countdown) and then asks RecordManager to start.
    void begin_recording_now();
    void set_start_pending(bool pending);

    RecordManager* m_rm;
    const VideoSettings& m_videoSettings;
    // RecordSettings is a plain member of AppSettings, not an element of a
    // vector, so this reference stays valid for the app's lifetime — none of
    // the reallocation hazards documented for VideoSettings::cameras apply.
    const RecordSettings& m_recordSettings;
    int m_cameraCount{0};
    QString m_sessionPath;
    VideoFeedProvider* m_feedProvider{nullptr};
    int m_frameGen{0};
    QVariantList m_frameGens;          // per-camera generation counters
    QTimer* m_countdownTimer{nullptr}; // repeating, 1 s; ticks the countdown down
    int m_countdownSeconds{0};         // 0 = no countdown pending
    bool m_startPending{false};        // click -> recording actually running
    bool m_hidePreviews{true};         // cached mirror of the record setting
};

} // namespace mosaic
