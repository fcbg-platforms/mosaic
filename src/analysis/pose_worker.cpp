#include "analysis/pose_worker.hpp"
#include "utils/logger.hpp"
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <cstring>

namespace mosaic {

struct PoseWorker::Impl {
    std::unique_ptr<QProcess> proc;
    QTimer*                   readTimer{nullptr};
    QByteArray                readBuf;
};

PoseWorker::PoseWorker(QObject* parent)
    : QObject(parent), d(std::make_unique<Impl>()) {}

PoseWorker::~PoseWorker() { stop(); }

bool PoseWorker::start(const QString& interpreter, const QString& scriptPath) {
    stop();

    d->proc = std::make_unique<QProcess>(this);
    d->proc->setProgram(interpreter);
    d->proc->setArguments({"-u", scriptPath});

    connect(d->proc.get(), &QProcess::readyReadStandardOutput, this, [this] {
        d->readBuf += d->proc->readAllStandardOutput();
        while (true) {
            const int nl = d->readBuf.indexOf('\n');
            if (nl < 0) break;
            const QByteArray line = d->readBuf.left(nl);
            d->readBuf.remove(0, nl + 1);

            const auto doc = QJsonDocument::fromJson(line);
            if (!doc.isObject()) continue;
            const auto obj = doc.object();

            const int camIdx  = obj["camera"].toInt();
            const auto kpArr  = obj["keypoints"].toArray();

            QVariantList kps;
            kps.reserve(kpArr.size());
            for (const auto& v : kpArr) {
                const auto kp = v.toObject();
                QVariantMap m;
                m["x"]          = kp["x"].toDouble();
                m["y"]          = kp["y"].toDouble();
                m["z"]          = kp["z"].toDouble();
                m["visibility"] = kp["visibility"].toDouble();
                m["name"]       = kp["name"].toString();
                kps.append(m);
            }
            emit pose_ready(camIdx, kps);

            if (!obj["gaze"].isNull() && obj["gaze"].isObject()) {
                const auto gz   = obj["gaze"].toObject();
                const auto fb   = gz["face_box"].toObject();
                const auto li   = gz["left_iris"].toObject();
                const auto ri   = gz["right_iris"].toObject();
                QVariantMap gazeMap;
                gazeMap["face_box"]  = QVariantMap{{"x", fb["x"].toDouble()},
                                                    {"y", fb["y"].toDouble()},
                                                    {"w", fb["w"].toDouble()},
                                                    {"h", fb["h"].toDouble()}};
                gazeMap["left_iris"] = QVariantMap{{"x", li["x"].toDouble()},
                                                    {"y", li["y"].toDouble()}};
                gazeMap["right_iris"]= QVariantMap{{"x", ri["x"].toDouble()},
                                                    {"y", ri["y"].toDouble()}};
                gazeMap["gaze_dx"]   = gz["gaze_dx"].toDouble();
                gazeMap["gaze_dy"]   = gz["gaze_dy"].toDouble();
                emit gaze_ready(camIdx, gazeMap);
            }
        }
    });

    connect(d->proc.get(), &QProcess::readyReadStandardError, this, [this] {
        const QString msg = QString::fromLocal8Bit(d->proc->readAllStandardError()).trimmed();
        if (!msg.isEmpty()) log_warning("[PoseWorker] " + msg);
    });

    connect(d->proc.get(), &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError err) {
        const QString msg = QString("Pose process error: %1").arg(static_cast<int>(err));
        log_error(msg);
        emit process_error(msg);
    });

    d->proc->start();
    if (!d->proc->waitForStarted(5000)) {
        log_error("[PoseWorker] Failed to start Python subprocess: " + d->proc->errorString());
        d->proc.reset();
        return false;
    }

    log_info("[PoseWorker] Python pose subprocess started — PID " +
             QString::number(d->proc->processId()));
    return true;
}

void PoseWorker::stop() {
    if (!d->proc) return;
    d->proc->closeWriteChannel();
    if (!d->proc->waitForFinished(3000))
        d->proc->kill();
    d->proc.reset();
}

bool PoseWorker::is_running() const {
    return d->proc && d->proc->state() == QProcess::Running;
}

void PoseWorker::submit_frame(int cameraIndex, QImage frame) {
    if (!is_running()) return;

    // Convert to BGR888 for the Python script.
    const QImage bgr = frame.convertToFormat(QImage::Format_BGR888);
    const int w = bgr.width();
    const int h = bgr.height();

    // Protocol: 16-byte header [cam_idx, w, h, 0] then raw BGR bytes.
    QByteArray header(16, 0);
    std::memcpy(header.data() + 0, &cameraIndex,    4);
    std::memcpy(header.data() + 4, &w,              4);
    std::memcpy(header.data() + 8, &h,              4);

    d->proc->write(header);
    for (int row = 0; row < h; ++row) {
        d->proc->write(reinterpret_cast<const char*>(bgr.scanLine(row)),
                       static_cast<qint64>(w * 3));
    }
}

} // namespace mosaic
