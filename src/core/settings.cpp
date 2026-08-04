#include "core/settings.hpp"
#include "trigger/trigger_types.hpp"
#include "utils/logger.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>

namespace mosaic {

// ── CalibrationData ────────────────────────────────────────────────────────

static QJsonArray array_to_json(const auto& arr) {
    QJsonArray out;
    for (const double v : arr) { out.append(v); }
    return out;
}

template<std::size_t N>
static std::array<double, N> json_to_array(const QJsonArray& ja,
                                             const std::array<double, N>& def) {
    if (static_cast<std::size_t>(ja.size()) != N) { return def; }
    std::array<double, N> out;
    for (std::size_t i = 0; i < N; ++i) { out[i] = ja[static_cast<int>(i)].toDouble(); }
    return out;
}

QJsonObject CalibrationData::to_json() const {
    return {
        {"calibrated",           calibrated},
        {"rms_error",            rmsError},
        {"camera_matrix",        array_to_json(cameraMatrix)},
        {"dist_coeffs",          array_to_json(distCoeffs)},
        {"extrinsic_rt",         array_to_json(extrinsicRt)},
        {"extrinsic_calibrated", extrinsicCalibrated},
    };
}

std::optional<CalibrationData> CalibrationData::from_json(const QJsonObject& o) {
    CalibrationData cal;
    if (o.contains("calibrated"))           { cal.calibrated          = o["calibrated"].toBool(); }
    if (o.contains("rms_error"))            { cal.rmsError            = o["rms_error"].toDouble(-1.0); }
    if (o.contains("camera_matrix"))        { cal.cameraMatrix        = json_to_array(o["camera_matrix"].toArray(), cal.cameraMatrix); }
    if (o.contains("dist_coeffs"))          { cal.distCoeffs          = json_to_array(o["dist_coeffs"].toArray(),   cal.distCoeffs); }
    if (o.contains("extrinsic_rt"))         { cal.extrinsicRt         = json_to_array(o["extrinsic_rt"].toArray(),  cal.extrinsicRt); }
    if (o.contains("extrinsic_calibrated")) { cal.extrinsicCalibrated = o["extrinsic_calibrated"].toBool(); }
    return cal;
}

// ── CameraParameters ───────────────────────────────────────────────────────

QJsonObject CameraParameters::to_json() const {
    return {
        {"serial",               serialNumber},
        {"name",                 friendlyName},
        {"width",                width},
        {"height",               height},
        {"offset_x",             offsetX},
        {"offset_y",             offsetY},
        {"reverse_x",            reverseX},
        {"reverse_y",            reverseY},
        {"pixel_format",         pixelFormat},
        {"specify_fps",          specifyFps},
        {"fps",                  fps},
        {"exposure_auto",        exposureAuto},
        {"exposure_time_us",     exposureTimeUs},
        {"exposure_auto_lower",  exposureAutoLowerUs},
        {"exposure_auto_upper",  exposureAutoUpperUs},
        {"gain_auto",            gainAuto},
        {"gain_db",              gainDb},
        {"gain_auto_lower",      gainAutoLowerDb},
        {"gain_auto_upper",      gainAutoUpperDb},
        {"gamma",                    gamma},
        {"black_level",              blackLevel},
        {"balance_white_auto",       balanceWhiteAuto},
        {"saturation",               saturation},
        {"contrast",                 contrast},
        {"brightness",               brightness},
        {"auto_target_brightness",   autoTargetBrightness},
        {"digital_shift",            digitalShift},
        {"hw_trigger_enabled",       hwTriggerEnabled},
        {"hw_trigger_source",        hwTriggerSource},
        {"hw_trigger_delay_us",      hwTriggerDelayUs},
        {"test_pattern",             testPattern},
        {"calibration",              calibration.to_json()},
    };
}

std::optional<CameraParameters> CameraParameters::from_json(const QJsonObject& o) {
    CameraParameters c;
    if (o.contains("serial"))              c.serialNumber        = o["serial"].toString();
    if (o.contains("name"))               c.friendlyName        = o["name"].toString(c.friendlyName);
    if (o.contains("width"))              c.width               = o["width"].toInt(c.width);
    if (o.contains("height"))             c.height              = o["height"].toInt(c.height);
    if (o.contains("offset_x"))           c.offsetX             = o["offset_x"].toInt(c.offsetX);
    if (o.contains("offset_y"))           c.offsetY             = o["offset_y"].toInt(c.offsetY);
    if (o.contains("reverse_x"))          c.reverseX            = o["reverse_x"].toBool(c.reverseX);
    if (o.contains("reverse_y"))          c.reverseY            = o["reverse_y"].toBool(c.reverseY);
    if (o.contains("pixel_format"))       c.pixelFormat         = o["pixel_format"].toString(c.pixelFormat);
    if (o.contains("specify_fps"))        c.specifyFps          = o["specify_fps"].toBool(c.specifyFps);
    if (o.contains("fps"))                c.fps                 = o["fps"].toDouble(c.fps);
    if (o.contains("exposure_auto"))      c.exposureAuto        = o["exposure_auto"].toString(c.exposureAuto);
    if (o.contains("exposure_time_us"))   c.exposureTimeUs      = o["exposure_time_us"].toDouble(c.exposureTimeUs);
    if (o.contains("exposure_auto_lower"))c.exposureAutoLowerUs = o["exposure_auto_lower"].toDouble(c.exposureAutoLowerUs);
    if (o.contains("exposure_auto_upper"))c.exposureAutoUpperUs = o["exposure_auto_upper"].toDouble(c.exposureAutoUpperUs);
    if (o.contains("gain_auto"))          c.gainAuto            = o["gain_auto"].toString(c.gainAuto);
    if (o.contains("gain_db"))            c.gainDb              = o["gain_db"].toDouble(c.gainDb);
    if (o.contains("gain_auto_lower"))    c.gainAutoLowerDb     = o["gain_auto_lower"].toDouble(c.gainAutoLowerDb);
    if (o.contains("gain_auto_upper"))    c.gainAutoUpperDb     = o["gain_auto_upper"].toDouble(c.gainAutoUpperDb);
    if (o.contains("gamma"))                    c.gamma               = o["gamma"].toDouble(c.gamma);
    if (o.contains("black_level"))              c.blackLevel          = o["black_level"].toDouble(c.blackLevel);
    if (o.contains("balance_white_auto"))       c.balanceWhiteAuto    = o["balance_white_auto"].toString(c.balanceWhiteAuto);
    if (o.contains("saturation"))               c.saturation          = o["saturation"].toDouble(c.saturation);
    if (o.contains("contrast"))                 c.contrast            = o["contrast"].toDouble(c.contrast);
    if (o.contains("brightness"))               c.brightness          = o["brightness"].toDouble(c.brightness);
    if (o.contains("auto_target_brightness"))   c.autoTargetBrightness= o["auto_target_brightness"].toDouble(c.autoTargetBrightness);
    if (o.contains("digital_shift"))            c.digitalShift        = o["digital_shift"].toInt(c.digitalShift);
    if (o.contains("hw_trigger_enabled"))       c.hwTriggerEnabled    = o["hw_trigger_enabled"].toBool(c.hwTriggerEnabled);
    if (o.contains("hw_trigger_source"))        c.hwTriggerSource     = o["hw_trigger_source"].toString(c.hwTriggerSource);
    if (o.contains("hw_trigger_delay_us"))      c.hwTriggerDelayUs    = o["hw_trigger_delay_us"].toDouble(c.hwTriggerDelayUs);
    if (o.contains("test_pattern"))             c.testPattern         = o["test_pattern"].toString(c.testPattern);
    if (o.contains("calibration")) {
        if (auto cal = CalibrationData::from_json(o["calibration"].toObject())) {
            c.calibration = std::move(*cal);
        }
    }
    return c;
}

// ── VideoSettings ──────────────────────────────────────────────────────────

QJsonObject VideoSettings::to_json() const {
    QJsonArray cams;
    for (const auto& c : cameras) cams.append(c.to_json());
    return {
        {"codec",      codec},
        {"preset",     preset},
        {"crf",        crf},
        {"bitrate",    bitrate},
        {"cameras",    cams},
    };
}

std::optional<VideoSettings> VideoSettings::from_json(const QJsonObject& o) {
    VideoSettings s;
    if (o.contains("codec"))      s.codec      = o["codec"].toString(s.codec);
    if (o.contains("preset"))     s.preset     = o["preset"].toString(s.preset);
    if (o.contains("crf"))        s.crf        = o["crf"].toInt(s.crf);
    if (o.contains("bitrate"))    s.bitrate    = o["bitrate"].toInt(s.bitrate);
    // Legacy "sync_fps"/"target_fps" keys from older settings.json files are
    // silently ignored — the feature was never wired into acquisition.
    if (o.contains("cameras")) {
        // Reserve up front so a caller that immediately hands this
        // VideoSettings to VideoManager::open() (see Application::
        // initialize()) gets the same reference-stability guarantee
        // VideoSettingsW's constructor already relies on — see
        // VideoSettings::kMaxCameras's doc comment.
        s.cameras.reserve(VideoSettings::kMaxCameras);
        for (const auto& v : o["cameras"].toArray()) {
            if (auto c = CameraParameters::from_json(v.toObject()))
                s.cameras.push_back(std::move(*c));
        }
    }
    return s;
}

// ── MicrophoneParameters ───────────────────────────────────────────────────

QJsonObject MicrophoneParameters::to_json() const {
    return {
        {"device_id",    deviceId},
        {"name",         friendlyName},
        {"sample_rate",  sampleRate},
        {"channels",     channels},
        {"buffer_size",  bufferSize},
    };
}

std::optional<MicrophoneParameters> MicrophoneParameters::from_json(const QJsonObject& o) {
    MicrophoneParameters m;
    if (o.contains("device_id"))   m.deviceId    = o["device_id"].toString();
    if (o.contains("name"))        m.friendlyName = o["name"].toString(m.friendlyName);
    if (o.contains("sample_rate")) m.sampleRate   = o["sample_rate"].toInt(m.sampleRate);
    if (o.contains("channels"))    m.channels     = o["channels"].toInt(m.channels);
    if (o.contains("buffer_size")) m.bufferSize   = o["buffer_size"].toInt(m.bufferSize);
    return m;
}

// ── AudioSettings ──────────────────────────────────────────────────────────

QJsonObject AudioSettings::to_json() const {
    QJsonArray mics;
    for (const auto& m : microphones) mics.append(m.to_json());
    return {{"codec", codec}, {"microphones", mics}};
}

std::optional<AudioSettings> AudioSettings::from_json(const QJsonObject& o) {
    AudioSettings s;
    if (o.contains("codec")) s.codec = o["codec"].toString(s.codec);
    if (o.contains("microphones")) {
        // Reserve up front — see AudioSettings::kMaxMicrophones's doc
        // comment (mirrors VideoSettings::from_json()'s identical fix).
        s.microphones.reserve(AudioSettings::kMaxMicrophones);
        for (const auto& v : o["microphones"].toArray()) {
            if (auto m = MicrophoneParameters::from_json(v.toObject()))
                s.microphones.push_back(std::move(*m));
        }
    }
    return s;
}

// ── SerialTriggerConfig ────────────────────────────────────────────────────

QJsonObject SerialTriggerConfig::to_json() const {
    return {
        {"name",        name},
        {"port_name",   portName},
        {"baud_rate",   baudRate},
        {"data_bits",   dataBits},
        {"parity",      parity},
        {"stop_bits",   stopBits},
        {"match_mode",  matchMode},
        {"match_value", matchValue},
        {"enabled",     enabled},
        {"action",      trigger_action_label(action)},
    };
}

std::optional<SerialTriggerConfig> SerialTriggerConfig::from_json(const QJsonObject& o) {
    SerialTriggerConfig c;
    if (o.contains("name"))        c.name       = o["name"].toString(c.name);
    if (o.contains("port_name"))   c.portName   = o["port_name"].toString(c.portName);
    if (o.contains("baud_rate"))   c.baudRate   = o["baud_rate"].toInt(c.baudRate);
    if (o.contains("data_bits"))   c.dataBits   = o["data_bits"].toInt(c.dataBits);
    if (o.contains("parity"))      c.parity     = o["parity"].toString(c.parity);
    if (o.contains("stop_bits"))   c.stopBits   = o["stop_bits"].toDouble(c.stopBits);
    if (o.contains("match_mode"))  c.matchMode  = o["match_mode"].toString(c.matchMode);
    if (o.contains("match_value")) c.matchValue = o["match_value"].toString(c.matchValue);
    if (o.contains("enabled"))     c.enabled    = o["enabled"].toBool(c.enabled);
    if (o.contains("action"))      c.action     = trigger_action_from_label(o["action"].toString());
    return c;
}

// ── ParallelPortConfig ─────────────────────────────────────────────────────

QJsonObject ParallelPortConfig::to_json() const {
    return {
        {"port_address",          portAddress},
        {"poll_rate_ms",          pollRateMs},
        {"enabled",               enabled},
        {"invert_logic",          invertLogic},
        {"action",                trigger_action_label(action)},
        {"send_recording_marker", sendRecordingMarker},
    };
}

std::optional<ParallelPortConfig> ParallelPortConfig::from_json(const QJsonObject& o) {
    ParallelPortConfig c;
    if (o.contains("port_address")) { c.portAddress = o["port_address"].toString(c.portAddress); }
    if (o.contains("poll_rate_ms")) { c.pollRateMs  = o["poll_rate_ms"].toInt(c.pollRateMs);     }
    if (o.contains("enabled"))      { c.enabled     = o["enabled"].toBool(c.enabled);             }
    if (o.contains("invert_logic")) { c.invertLogic = o["invert_logic"].toBool(c.invertLogic);   }
    if (o.contains("action"))       { c.action      = trigger_action_from_label(o["action"].toString()); }
    if (o.contains("send_recording_marker")) {
        c.sendRecordingMarker = o["send_recording_marker"].toBool(c.sendRecordingMarker);
    }
    return c;
}

// ── KeyTriggerConfig ───────────────────────────────────────────────────────

QJsonObject KeyTriggerConfig::to_json() const {
    return {{"name", name}, {"key_seq", keySeq}, {"enabled", enabled},
            {"action", trigger_action_label(action)}};
}

std::optional<KeyTriggerConfig> KeyTriggerConfig::from_json(const QJsonObject& o) {
    KeyTriggerConfig c;
    if (o.contains("name"))    c.name    = o["name"].toString(c.name);
    if (o.contains("key_seq")) c.keySeq  = o["key_seq"].toString(c.keySeq);
    if (o.contains("enabled")) c.enabled = o["enabled"].toBool(c.enabled);
    if (o.contains("action"))  c.action  = trigger_action_from_label(o["action"].toString());
    return c;
}

// ── TriggerSettings ────────────────────────────────────────────────────────

QJsonObject TriggerSettings::to_json() const {
    QJsonArray keys, serials, ports;
    for (const auto& k : keyboardTriggers) { keys.append(k.to_json()); }
    for (const auto& s : serialTriggers)   { serials.append(s.to_json()); }
    for (const auto& p : parallelPorts)    { ports.append(p.to_json()); }
    return {
        {"receive_enabled",    receiveEnabled},
        {"keyboard_triggers",  keys},
        {"serial_triggers",    serials},
        {"parallel_ports",     ports},
    };
}

std::optional<TriggerSettings> TriggerSettings::from_json(const QJsonObject& o) {
    TriggerSettings s;
    if (o.contains("receive_enabled"))    { s.receiveEnabled   = o["receive_enabled"].toBool(s.receiveEnabled);       }
    if (o.contains("keyboard_triggers")) {
        for (const auto& v : o["keyboard_triggers"].toArray()) {
            if (auto c = KeyTriggerConfig::from_json(v.toObject())) { s.keyboardTriggers.push_back(std::move(*c)); }
        }
    }
    if (o.contains("serial_triggers")) {
        for (const auto& v : o["serial_triggers"].toArray()) {
            if (auto c = SerialTriggerConfig::from_json(v.toObject())) { s.serialTriggers.push_back(std::move(*c)); }
        }
    }
    if (o.contains("parallel_ports")) {
        for (const auto& v : o["parallel_ports"].toArray()) {
            if (auto c = ParallelPortConfig::from_json(v.toObject())) { s.parallelPorts.push_back(std::move(*c)); }
        }
    }
    return s;
}

// ── RecordSettings ─────────────────────────────────────────────────────────

QJsonObject RecordSettings::to_json() const {
    return {
        {"directory",        directory},
        {"video_basename",   videoBasename},
        {"audio_basename",   audioBasename},
        {"trigger_basename", triggerBasename},
        {"separator",        separator},
        {"add_timestamp",    addTimestamp},
        {"timestamp_format", timestampFormat},
        {"enable_video",     enableVideo},
        {"enable_audio",     enableAudio},
        {"enable_trigger",   enableTrigger},
    };
}

std::optional<RecordSettings> RecordSettings::from_json(const QJsonObject& o) {
    RecordSettings s;
    if (o.contains("directory"))        s.directory       = o["directory"].toString(s.directory);
    if (o.contains("video_basename"))   s.videoBasename   = o["video_basename"].toString(s.videoBasename);
    if (o.contains("audio_basename"))   s.audioBasename   = o["audio_basename"].toString(s.audioBasename);
    if (o.contains("trigger_basename")) s.triggerBasename = o["trigger_basename"].toString(s.triggerBasename);
    if (o.contains("separator"))        s.separator       = o["separator"].toString(s.separator);
    if (o.contains("add_timestamp"))    s.addTimestamp    = o["add_timestamp"].toBool(s.addTimestamp);
    if (o.contains("timestamp_format")) s.timestampFormat = o["timestamp_format"].toString(s.timestampFormat);
    if (o.contains("enable_video"))     s.enableVideo     = o["enable_video"].toBool(s.enableVideo);
    if (o.contains("enable_audio"))     s.enableAudio     = o["enable_audio"].toBool(s.enableAudio);
    if (o.contains("enable_trigger"))   s.enableTrigger   = o["enable_trigger"].toBool(s.enableTrigger);
    return s;
}

QJsonObject AnalysisSettings::to_json() const {
    return {
        {"hf_token", hfToken},
    };
}

std::optional<AnalysisSettings> AnalysisSettings::from_json(const QJsonObject& o) {
    AnalysisSettings s;
    if (o.contains("hf_token")) s.hfToken = o["hf_token"].toString(s.hfToken);
    return s;
}

// ── RoomSettings ───────────────────────────────────────────────────────────

QJsonObject RoomSettings::to_json() const {
    return {
        {"plane_point",   array_to_json(planePoint)},
        {"plane_normal",  array_to_json(planeNormal)},
        {"plane_defined", planeDefined},
    };
}

std::optional<RoomSettings> RoomSettings::from_json(const QJsonObject& o) {
    RoomSettings s;
    if (o.contains("plane_point"))   { s.planePoint   = json_to_array(o["plane_point"].toArray(),  s.planePoint); }
    if (o.contains("plane_normal"))  { s.planeNormal  = json_to_array(o["plane_normal"].toArray(), s.planeNormal); }
    if (o.contains("plane_defined")) { s.planeDefined = o["plane_defined"].toBool(); }
    return s;
}

// ── AppSettings ────────────────────────────────────────────────────────────

bool AppSettings::save(const QString& path) const {
    const QJsonObject root{
        {"schema_version", k_schema_version},
        {"video",          video.to_json()},
        {"audio",          audio.to_json()},
        {"trigger",        trigger.to_json()},
        {"record",         record.to_json()},
        {"analysis",       analysis.to_json()},
        {"room",           room.to_json()},
    };

    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        log_error(QString("Cannot create config directory: %1").arg(QFileInfo(path).absolutePath()));
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        log_error(QString("Cannot write settings to: %1 — %2").arg(path, file.errorString()));
        return false;
    }

    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    log_info(QString("Settings saved → %1").arg(path));
    return true;
}

std::optional<AppSettings> AppSettings::load(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        log_info(QString("No settings file at %1 — using defaults").arg(path));
        return AppSettings{};
    }

    QJsonParseError err;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError) {
        log_error(QString("Settings parse error in %1: %2").arg(path, err.errorString()));
        return std::nullopt;
    }

    const auto root = doc.object();
    AppSettings s;
    if (root.contains("video"))
        s.video   = VideoSettings::from_json(root["video"].toObject()).value_or(s.video);
    if (root.contains("audio"))
        s.audio   = AudioSettings::from_json(root["audio"].toObject()).value_or(s.audio);
    if (root.contains("trigger"))
        s.trigger = TriggerSettings::from_json(root["trigger"].toObject()).value_or(s.trigger);
    if (root.contains("record"))
        s.record  = RecordSettings::from_json(root["record"].toObject()).value_or(s.record);
    if (root.contains("analysis"))
        s.analysis = AnalysisSettings::from_json(root["analysis"].toObject()).value_or(s.analysis);
    if (root.contains("room"))
        s.room    = RoomSettings::from_json(root["room"].toObject()).value_or(s.room);

    log_info(QString("Settings loaded ← %1").arg(path));
    return s;
}

QString AppSettings::default_path() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
           + "/settings.json";
}

} // namespace mosaic
