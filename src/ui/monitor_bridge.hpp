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

    // ── Session identity ───────────────────────────────────────────────────
    //
    // Who and what the next recording is of. All optional: left empty, the
    // session keeps the timestamp-only folder name this app has always used.
    //
    // These hold the operator's text *verbatim*, not the sanitized label.
    // Sanitizing on every keystroke would delete characters from under the
    // cursor as they typed (a hyphen, a space), which is hostile; instead the
    // raw text stays put, folderPreview shows what will actually be created,
    // and identityWarning says so in words when the two differ.
    Q_PROPERTY(QString subjectLabel READ subjectLabel WRITE setSubjectLabel NOTIFY identityChanged)
    Q_PROPERTY(QString sessionLabel READ sessionLabel WRITE setSessionLabel NOTIFY identityChanged)
    Q_PROPERTY(QString taskLabel READ taskLabel WRITE setTaskLabel NOTIFY identityChanged)
    // Free-text operator note, saved beside the recording as notes.txt.
    // Editable while recording too — the note worth having is usually the one
    // written once something has actually happened.
    Q_PROPERTY(QString notes READ notes WRITE setNotes NOTIFY notesChanged)
    // The folder name that would be created if Record were clicked right now,
    // run index included. The point is that the operator never has to guess.
    Q_PROPERTY(QString folderPreview READ folderPreview NOTIFY identityChanged)
    // One line, empty when there is nothing to say: what got dropped from a
    // label, or that the name is too long for the recordings directory.
    Q_PROPERTY(QString identityWarning READ identityWarning NOTIFY identityChanged)

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
    [[nodiscard]] QString subjectLabel() const;
    [[nodiscard]] QString sessionLabel() const;
    [[nodiscard]] QString taskLabel() const;
    [[nodiscard]] QString notes() const;
    [[nodiscard]] QString folderPreview() const;
    [[nodiscard]] QString identityWarning() const;

    void setSubjectLabel(const QString& v);
    void setSessionLabel(const QString& v);
    void setTaskLabel(const QString& v);
    void setNotes(const QString& v);

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

    // Answers to the duplicate-name question MainWindow asks on our behalf
    // (see runCollisionDetected). Exactly one of these must be called for
    // every signal emitted, or Record stays deaf.
    Q_INVOKABLE void confirmRunAndStart(int run);
    Q_INVOKABLE void cancelPendingStart();

    // Clears subject/session/task/notes.
    Q_INVOKABLE void clearIdentity();

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
    void identityChanged();
    void notesChanged();

    // This subject/session/task already has recordings. Emitted *instead of*
    // arming the countdown, so the operator is asked before anything happens
    // rather than after a 3-2-1. MainWindow shows the dialog: a QML popup
    // cannot be used here because the project targets Qt 6.4, where a Popup is
    // clipped to its QQuickWidget — a window the user can drag arbitrarily
    // small — and a question governing where data lands must not be croppable.
    void runCollisionDetected(QString entityPrefix, int existingCount, int suggestedRun);

   private:
    // Clears the countdown (notifying QML first, so a failed start can never
    // strand the UI mid-countdown) and then asks RecordManager to start.
    void begin_recording_now();
    void set_start_pending(bool pending);

    // The tail of startRecording(): hand the identity to RecordManager and
    // arm the countdown. Split out so the collision answer can rejoin the
    // normal path rather than duplicating it.
    void arm_countdown(const SessionIdentity& id);

    // Current field text as an identity. Labels are still raw here; every
    // consumer in session_name.hpp sanitizes what it is given.
    [[nodiscard]] SessionIdentity current_identity() const;

    // Hands the current field contents to RecordManager. Called on every
    // edit, not just at Record time — see the implementation.
    void publish_identity();

    // Recomputes the cached preview + warning with a single scan of the
    // recordings directory. Previously folderPreview() and identityWarning()
    // each scanned it, and the latter called the former, so every keystroke
    // cost two or three synchronous directory listings on the thread also
    // driving the camera previews.
    void recompute_identity_preview();

    void flush_notes_to_disk() const;

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

    QString m_subjectLabel;
    QString m_sessionLabel;
    QString m_taskLabel;
    QString m_notes;
    // True from emitting runCollisionDetected until the answer arrives.
    // Without it a second Record click would stack a second dialog behind the
    // first — QMessageBox::exec() spins the event loop, so clicks keep being
    // delivered while it is open.
    bool m_awaitingCollisionAnswer{false};
    SessionIdentity m_collisionIdentity;
    // Debounces notes typed *during* a recording; see flush_notes_to_disk().
    QTimer* m_notesFlushTimer{nullptr};
    // Cached by recompute_identity_preview(); m_resolvedIdentity carries the
    // run index so the pre-flight length check measures the name that will
    // actually be created, not one that is missing "_run-NN".
    SessionIdentity m_resolvedIdentity;
    QString m_folderPreview;
    QString m_identityWarning;
};

} // namespace mosaic
