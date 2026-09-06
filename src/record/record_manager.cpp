#include "record/record_manager.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTimer>

#include "utils/logger.hpp"
#include "utils/timestamp.hpp"

namespace mosaic {

struct RecordManager::Impl {
    AppSettings& settings;
    TriggerManager* triggerMgr;
    AudioManager* audioMgr;
    VideoManager* videoMgr;
    QString username;

    bool recording{false};
    int elapsedMs{0};
    int64_t startMs{0};
    QString sessionPath;
    QTimer timer;

    // Armed by set_session_identity(); consumed by start(). `active` is the
    // resolved copy — same labels, but with the run index that was actually
    // free when the folder was created — and is what gets written into
    // session_meta.json.
    SessionIdentity pendingIdentity;
    SessionIdentity activeIdentity;

    explicit Impl(AppSettings& s, TriggerManager* tr, AudioManager* au, VideoManager* vi,
                  const QString& user)
        : settings(s), triggerMgr(tr), audioMgr(au), videoMgr(vi), username(user) {}
};

RecordManager::RecordManager(AppSettings& settings, TriggerManager* triggerMgr,
                             AudioManager* audioMgr, VideoManager* videoMgr,
                             const QString& username, QObject* parent)
    : QObject(parent),
      d(std::make_unique<Impl>(settings, triggerMgr, audioMgr, videoMgr, username)) {
    d->timer.setInterval(100);
    connect(&d->timer, &QTimer::timeout, this, &RecordManager::tick);
}

RecordManager::~RecordManager() { stop(); }

// ── Session metadata ────────────────────────────────────────────────────────

void RecordManager::write_session_meta() const {
    const QString path = d->sessionPath + "/session_meta.json";

    // Cameras
    QJsonArray cameras;
    for (int i = 0; i < static_cast<int>(d->settings.video.cameras.size()); ++i) {
        const auto& cam          = d->settings.video.cameras[static_cast<size_t>(i)];
        const QJsonObject calObj = cam.calibration.to_json();
        cameras.append(QJsonObject{
            {"index", i},
            {"serial", cam.serialNumber},
            {"name", cam.friendlyName},
            {"width", cam.width},
            {"height", cam.height},
            {"fps", cam.fps},
            {"pixel_format", cam.pixelFormat},
            {"codec", d->settings.video.codec},
            {"calibration", calObj},
        });
    }

    // Microphones
    QJsonArray mics;
    for (int i = 0; i < static_cast<int>(d->settings.audio.microphones.size()); ++i) {
        const auto& mic = d->settings.audio.microphones[static_cast<size_t>(i)];
        mics.append(QJsonObject{
            {"index", i},
            {"device_id", mic.deviceId},
            {"name", mic.friendlyName},
            {"sample_rate", mic.sampleRate},
            {"channels", mic.channels},
        });
    }

    // Trigger sources
    QJsonArray keys, ports;
    for (const auto& k : d->settings.trigger.keyboardTriggers) {
        // `code` makes the session self-describing: trigger.csv logs numeric
        // codes, and an analyst reading it later shouldn't need access to the
        // recording machine's settings.json to learn what each one meant.
        keys.append(QJsonObject{{"name", k.name}, {"key_seq", k.keySeq}, {"code", k.code}});
    }
    for (const auto& p : d->settings.trigger.parallelPorts) {
        ports.append(QJsonObject{{"port_address", p.portAddress}});
    }

    QJsonObject root{
        {"schema", "mosaic-session-v1"},
        {"mosaic_version", QCoreApplication::applicationVersion()},
        {"recorded_by", d->username},
        {"session_start_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"session_start_elapsed_ns", elapsed_ns()},
        {"session_folder", d->sessionPath},
        {"cameras", cameras},
        {"microphones", mics},
        {"room", d->settings.room.to_json()},
        {"trigger_sources",
         QJsonObject{
             {"keyboard", keys},
             {"parallel_ports", ports},
         }},
        {"recording",
         QJsonObject{
             {"video_enabled", d->settings.record.enableVideo},
             {"audio_enabled", d->settings.record.enableAudio},
             {"trigger_enabled", d->settings.record.enableTrigger},
             {"video_codec", d->settings.video.codec},
             {"audio_codec", d->settings.audio.codec},
         }},
    };

    // Added only when the operator actually labelled the recording, so an
    // unlabelled session's metadata is byte-for-byte what it always was —
    // absent, rather than present-and-null, which a reader would have to
    // special-case. Purely additive, so no schema bump: every reader looks
    // keys up by name, and nothing anywhere validates "schema".
    //
    // The run index goes in even though the folder name also carries it. This
    // is where a tool should read identity from, so that nothing downstream
    // ever has to parse a directory name.
    if (d->activeIdentity.has_entities()) {
        root.insert("bids", d->activeIdentity.to_json());
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        log_error(QString("[RecordManager] Cannot write session metadata: %1").arg(path));
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    log_info(QString("[RecordManager] Session metadata → %1").arg(path));
}

// ── Start ──────────────────────────────────────────────────────────────────

void RecordManager::set_session_identity(const SessionIdentity& id) {
    if (d->recording) {
        return; // a session's identity is fixed once its folder exists
    }
    d->pendingIdentity = id;
}

SessionIdentity RecordManager::session_identity() const { return d->pendingIdentity; }

bool RecordManager::start() {
    if (d->recording) {
        log_warning("[RecordManager] start() called while already recording.");
        return false;
    }

    // 1. Resolve the identity, then build and create the session folder.
    //
    // The run index is resolved here rather than in the UI because start()
    // has a caller with no UI at all (a StartRecording trigger). Both paths
    // therefore number repeats the same way; the pre-flight scan behind the
    // operator's collision dialog is only advisory, and this one decides.
    d->activeIdentity = d->pendingIdentity;
    if (d->activeIdentity.has_entities()) {
        d->activeIdentity.run =
            next_run_index(existing_session_names(d->settings.record.directory), d->activeIdentity);
    }

    // Enforced here, not only in the UI: a StartRecording trigger reaches
    // start() directly with the identity prefilled from the last session, so
    // a check that lives only in MonitorBridge would not cover it. Refusing is
    // right — an over-budget folder records fine and then fails silently later,
    // when a plugin cannot create its output inside it.
    if (!fits_path_budget(d->settings.record.directory,
                          build_session_folder_name(d->activeIdentity, QDateTime::currentDateTime(),
                                                    d->settings.record.addTimestamp
                                                        ? d->settings.record.timestampFormat
                                                        : QString()))) {
        const QString msg =
            QString(
                "Session name is too long for %1 — shorten the subject, session or task "
                "label before recording.")
                .arg(d->settings.record.directory);
        log_error(msg);
        emit error_occurred(msg);
        return false;
    }

    d->sessionPath = create_session_folder();
    if (d->sessionPath.isEmpty()) {
        const QString msg =
            QString("Cannot create session folder under: %1").arg(d->settings.record.directory);
        log_error(msg);
        emit error_occurred(msg);
        return false;
    }
    log_info(QString("[RecordManager] Session folder: %1").arg(d->sessionPath));

    // Remember the labels (not the run index, which is derived, and not the
    // notes, which describe this recording only) so the next session prefills.
    d->settings.record.lastIdentity.subject = sanitize_label(d->activeIdentity.subject);
    d->settings.record.lastIdentity.session = sanitize_label(d->activeIdentity.session);
    d->settings.record.lastIdentity.task    = sanitize_label(d->activeIdentity.task);

    // 2. Write session metadata immediately so it exists even if recording fails.
    write_session_meta();
    write_session_notes();

    // 3. Trigger CSV.
    if (d->settings.record.enableTrigger && d->triggerMgr) {
        const QString path = build_file_path(d->settings.record.triggerBasename, "csv");
        d->triggerMgr->start_recording(path);
    }

    // 4. Audio recording — written under sessionPath/audio/, not the session
    // root, so a session folder separates media by kind. A failure to create
    // that subfolder must not silently record zero audio while still
    // reporting a successful start — skip audio and surface the error
    // instead, same severity as the session-folder failure above.
    if (d->settings.record.enableAudio && d->audioMgr && !d->settings.audio.microphones.empty()) {
        const QString audioDir = d->sessionPath + "/audio";
        if (!QDir().mkpath(audioDir)) {
            const QString msg = QString("Cannot create audio folder: %1").arg(audioDir);
            log_error(msg);
            emit error_occurred(msg);
        } else {
            d->audioMgr->start(audioDir, d->settings.record.audioBasename,
                               d->settings.audio.microphones);
        }
    }

    // 5. Video recording — written under sessionPath/video/, same rationale.
    if (d->settings.record.enableVideo && d->videoMgr) {
        const QString videoDir = d->sessionPath + "/video";
        if (!QDir().mkpath(videoDir)) {
            const QString msg = QString("Cannot create video folder: %1").arg(videoDir);
            log_error(msg);
            emit error_occurred(msg);
        } else {
            d->videoMgr->start(videoDir, d->settings.record.videoBasename, d->settings.video);
        }
    } else if (d->settings.record.enableVideo) {
        for (int i = 0; i < static_cast<int>(d->settings.video.cameras.size()); ++i) {
            log_info(QString("[RecordManager] Video backend not available for camera %1").arg(i));
        }
    }

    // 6. Start elapsed timer.
    d->startMs   = QDateTime::currentMSecsSinceEpoch();
    d->elapsedMs = 0;
    d->recording = true;
    d->timer.start();

    log_info("[RecordManager] Recording started.");
    emit recording_started(d->sessionPath);
    return true;
}

// ── Stop ───────────────────────────────────────────────────────────────────

void RecordManager::stop() {
    if (!d->recording) {
        return;
    }

    d->timer.stop();
    d->recording = false;

    const int duration = d->elapsedMs;
    const QString path = d->sessionPath;

    // Stop subsystems in reverse order.
    if (d->settings.record.enableVideo && d->videoMgr) {
        d->videoMgr->stop();
    }

    if (d->settings.record.enableAudio && d->audioMgr) {
        d->audioMgr->stop();
    }

    if (d->settings.record.enableTrigger && d->triggerMgr) {
        d->triggerMgr->stop_recording();
    }

    log_info(QString("[RecordManager] Recording stopped. Duration: %1 ms. Files in: %2")
                 .arg(duration)
                 .arg(path));

    emit recording_stopped(path, duration);
}

// ── Timer tick ─────────────────────────────────────────────────────────────

void RecordManager::tick() {
    d->elapsedMs = static_cast<int>(QDateTime::currentMSecsSinceEpoch() - d->startMs);
    emit elapsed_ms_changed(d->elapsedMs);
}

// ── Accessors ──────────────────────────────────────────────────────────────

bool RecordManager::is_recording() const { return d->recording; }
int RecordManager::elapsed_ms() const { return d->elapsedMs; }
QString RecordManager::current_session_path() const { return d->sessionPath; }

// ── Path helpers ───────────────────────────────────────────────────────────

void RecordManager::write_session_notes() const {
    const QString notes = d->activeIdentity.notes.trimmed();
    if (notes.isEmpty()) {
        return; // don't litter empty files
    }
    // A plain .txt sidecar rather than a key in session_meta.json. That file is
    // written once, before any data exists, and its value is being a frozen
    // record of the moment of recording — it holds the calibration matrices and
    // the time origins. Notes have to stay editable afterwards (the useful one
    // is usually written after the session, not before), and rewriting the
    // provenance file to fix a typo risks taking the extrinsics with it. This
    // mirrors the split the codebase already makes between session_meta.json
    // and the mutable annotations.json.
    QFile f(d->sessionPath + "/notes.txt");
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        f.write(notes.toUtf8());
    } else {
        log_warning(QString("[RecordManager] Could not write session notes to %1")
                        .arg(d->sessionPath + "/notes.txt"));
    }
}

QString RecordManager::build_session_path() const {
    const auto& rec = d->settings.record;
    QString path    = rec.directory;
    if (!path.endsWith('/')) {
        path += '/';
    }
    // An empty format means "no timestamp" to build_session_folder_name(),
    // which is how RecordSettings::addTimestamp reaches it.
    path += build_session_folder_name(d->activeIdentity, QDateTime::currentDateTime(),
                                      rec.addTimestamp ? rec.timestampFormat : QString());
    return QDir::cleanPath(path);
}

QString RecordManager::create_session_folder() const {
    // Reads d->activeIdentity, which start() has already resolved.
    const QString wanted = build_session_path();
    const QFileInfo info(wanted);
    const QString parent = info.path();
    const QString leaf   = info.fileName();

    if (!QDir().mkpath(parent)) {
        return {};
    }

    // mkdir(), not mkpath(), and this distinction is the whole point:
    // mkpath() returns true when the directory already exists, so the previous
    // code silently recorded *into* a previous session — truncating its
    // trigger.csv, overwriting its session_meta.json and its video files, with
    // no warning anywhere. That was reachable in normal use: with
    // addTimestamp off the folder is always the literal "session". mkdir()
    // fails on an existing directory instead, which also closes the
    // exists()-then-create race.
    QDir parentDir(parent);
    if (parentDir.mkdir(leaf)) {
        return wanted;
    }

    // Taken. Never overwrite and never refuse — a recording in progress is not
    // the moment to argue about names, so take the next free suffix and say so.
    for (int suffix = 2; suffix < 1000; ++suffix) {
        const QString candidate = QString("%1_%2").arg(leaf).arg(suffix);
        if (parentDir.mkdir(candidate)) {
            log_warning(QString("[RecordManager] Session folder %1 already existed; "
                                "recording into %2 instead so nothing is overwritten.")
                            .arg(leaf, candidate));
            return QDir::cleanPath(parent + "/" + candidate);
        }
    }
    return {};
}

QString RecordManager::build_file_path(const QString& basename, const QString& ext) const {
    return d->sessionPath + "/" + basename + "." + ext;
}

} // namespace mosaic
