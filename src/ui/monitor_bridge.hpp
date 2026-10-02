#pragma once
#include <QImage>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include "core/settings.hpp"
#include "record/record_manager.hpp"
#include "session/session_name.hpp"
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

    // ── Interview mode ─────────────────────────────────────────────────────
    // Mirrors of VideoSettings::interview, refreshed by refresh_video_settings()
    // after every switch. cameraCount deliberately keeps meaning "configured
    // cameras" in interview mode too: tiles are addressed by configured index
    // (the videofeed URL, frameGens[]), so the view picks which index to show
    // rather than the count shrinking under it — see MonitorView.qml.
    Q_PROPERTY(bool interviewMode READ interviewMode NOTIFY interviewChanged)
    Q_PROPERTY(int interviewCameraIndex READ interviewCameraIndex NOTIFY interviewChanged)
    Q_PROPERTY(double interviewFps READ interviewFps NOTIFY interviewChanged)
    // True while the cameras are being closed and reopened for a switch,
    // which takes a few seconds on real hardware; the toggle shows it and
    // refuses a second request meanwhile.
    Q_PROPERTY(bool interviewSwitching READ interviewSwitching NOTIFY interviewChanged)

    // ── Session identity ───────────────────────────────────────────────────
    //
    // Who and what the next recording is of. A subject is required to record by
    // hand: clicking Record without one opens a dialog asking for it rather
    // than starting. Session and task stay optional. A recording started by an
    // external trigger can still land with none of them set, and then keeps the
    // timestamp-only folder name this app has always used.
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
    // label, that a subject is still missing, or that the name is too long for
    // the recordings directory.
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
    [[nodiscard]] bool interviewMode() const;
    [[nodiscard]] int interviewCameraIndex() const;
    [[nodiscard]] double interviewFps() const;
    [[nodiscard]] bool interviewSwitching() const;
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

    // Re-reads VideoSettings::interview. Called by MainWindow once a mode
    // switch has been applied (or refused), never on raw edits in the
    // settings tab: the monitor shows the mode that is open, not one staged.
    void refresh_video_settings();

    // Set by MainWindow around the close/reopen a switch needs.
    void set_interview_switching(bool switching);

    // Starts a recording on behalf of an external StartRecording trigger.
    //
    // Deliberately not startRecording(): a trigger gets no countdown, because
    // that delay is an affordance for a human who has just clicked and a
    // trigger's timing belongs to the experiment; no duplicate-name dialog,
    // because nobody is at the keyboard to answer one; and no subject
    // requirement, because refusing here would turn a mislabelled recording
    // into no recording at all, which is strictly worse. What it does add over
    // the old route — Application calling RecordManager::start() directly — is
    // that a trigger firing with no subject now says so in the log, and that
    // there is one place describing what starting a recording means.
    //
    // Reached from Application's action_requested handler via
    // MainWindow::start_recording_from_trigger(), for the same reason
    // cancel_countdown() is: that handler only sees RecordManager.
    void start_from_trigger();

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

    // Answers to the question MainWindow asks on our behalf (see
    // identityConfirmationNeeded).
    //
    // Three strings rather than a SessionIdentity on purpose: that struct also
    // carries the operator's notes, and accepting one built from the dialog's
    // three fields would silently wipe whatever is in the notes box.
    // Returns false when the labels were saved but no recording could be
    // started — which happens when a trigger started one while the dialog was
    // open. The caller is expected to say so on screen: an accepted dialog that
    // silently records nothing is indistinguishable from a bug.
    bool confirmIdentityAndStart(const QString& subject, const QString& session,
                                 const QString& task);
    Q_INVOKABLE void cancelPendingStart();

    // Clears subject/session/task/notes.
    Q_INVOKABLE void clearIdentity();

    // The header toggle. Only asks: MainWindow owns the camera reopen and
    // refuses while a recording runs or is about to start. Ignored while a
    // switch is already in flight.
    Q_INVOKABLE void requestInterviewMode(bool on);

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
    void interviewChanged();

    // See requestInterviewMode(). Connected queued by MainWindow, so the
    // reopen never runs inside QML's delivery of the click that asked for it.
    void interviewModeRequested(bool on);

    // This recording cannot start on what has been typed so far, or would
    // repeat a combination already on disk. Emitted *instead of* arming the
    // countdown, so the operator is asked before anything happens rather than
    // after a 3-2-1.
    //
    // MainWindow shows the dialog: a QML popup cannot be used here because the
    // project targets Qt 6.4, where a Popup is clipped to its QQuickWidget — a
    // window the user can drag arbitrarily small — and a question governing
    // where data lands must not be croppable.
    //
    // The three labels ride along rather than being read back off this object,
    // so the dialog has everything it needs to prefill itself from the signal
    // alone. Exactly one of confirmIdentityAndStart() or cancelPendingStart()
    // must follow, or Record stays deaf.
    void identityConfirmationNeeded(QString subject, QString session, QString task);

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
    bool m_interviewMode{false};       // cached mirrors of VideoSettings::interview
    int m_interviewCameraIndex{0};
    double m_interviewFps{0.0};
    bool m_interviewSwitching{false};

    QString m_subjectLabel;
    QString m_sessionLabel;
    QString m_taskLabel;
    QString m_notes;
    // True from emitting identityConfirmationNeeded() until the answer arrives.
    // Without it a second Record click would stack a second dialog behind the
    // first — QDialog::exec() spins the event loop, so clicks keep being
    // delivered while it is open.
    bool m_awaitingConfirmation{false};
    // Debounces notes typed *during* a recording; see flush_notes_to_disk().
    QTimer* m_notesFlushTimer{nullptr};
    // Everything the UI says about the current three labels, refreshed by
    // recompute_identity_preview() on every edit. One struct rather than four
    // parallel members, so the preview, the warning and "may this record" are
    // decided in a single pass and cannot drift out of agreement.
    IdentityAdvice m_advice;
};

} // namespace mosaic
