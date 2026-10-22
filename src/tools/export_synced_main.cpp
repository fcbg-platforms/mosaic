// mosaic_export_synced: makes a session's synced videos from Frame Sync's
// alignment (synced/sync_repair.json and synced/video_N.repair_map.csv),
// with MOSAIC's own encoder. MOSAIC's Analysis tab runs it like an analysis
// script (AnalysisManager), so it is queued, held while recording and shown
// with the same progress lines:
//
//   mosaic_export_synced --session DIR [--camera N ...]
//
// Writes synced/video_N.mp4 per camera. Exit code 0 when every camera asked
// for was written, 1 otherwise. See docs/math/frame_sync.rst.

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <cstdio>

#include "video/synced_video_export.hpp"

using namespace mosaic;

namespace {

const char* kTag = "[mosaic_export_synced]";

void say(const QString& line) {
    std::fprintf(stdout, "%s\n", line.toUtf8().constData());
    std::fflush(stdout);
}

void complain(const QString& line) {
    std::fprintf(stderr, "%s %s\n", kTag, line.toUtf8().constData());
    std::fflush(stderr);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    QString session;
    QSet<int> only;
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "--session" && i + 1 < args.size()) {
            session = args[++i];
        } else if (args[i] == "--camera") {
            while (i + 1 < args.size() && !args[i + 1].startsWith("--")) {
                only.insert(args[++i].toInt());
            }
        }
    }
    if (session.isEmpty()) {
        complain("usage: mosaic_export_synced --session DIR [--camera N ...]");
        return 2;
    }
    const QDir dir(session);
    QFile summaryFile(dir.filePath("synced/sync_repair.json"));
    if (!summaryFile.open(QIODevice::ReadOnly)) {
        complain("No Frame Sync for this session (synced/sync_repair.json): run Frame Sync first.");
        return 1;
    }
    const QJsonObject summary = QJsonDocument::fromJson(summaryFile.readAll()).object();
    const double fps          = summary["master_fps"].toDouble();
    if (fps <= 0) {
        complain("synced/sync_repair.json has no frame rate.");
        return 1;
    }

    struct Job {
        int camera;
        QString source;
        QString map;
        QString out;
    };
    QList<Job> jobs;
    for (const auto& v : summary["cameras"].toArray()) {
        const QJsonObject cam = v.toObject();
        const int index       = cam["index"].toInt();
        if (cam["skipped"].toBool() || (!only.isEmpty() && !only.contains(index))) {
            continue;
        }
        const QString name = QString("video_%1").arg(index);
        jobs.append({index, dir.filePath(cam["source_video"].toString()),
                     dir.filePath("synced/" + name + ".repair_map.csv"),
                     dir.filePath("synced/" + name + ".mp4")});
    }
    if (jobs.isEmpty()) {
        complain("No camera to export.");
        return 1;
    }

    int failed = 0;
    for (int j = 0; j < jobs.size(); ++j) {
        const Job& job = jobs[j];
        say(QString("%1 Camera %2/%3: %4")
                .arg(kTag)
                .arg(j + 1)
                .arg(jobs.size())
                .arg(QFileInfo(job.source).fileName()));
        QString error;
        const auto rows = read_repair_map(job.map, &error);
        if (rows.empty()) {
            complain(QString("Camera %1: %2").arg(job.camera + 1).arg(error));
            ++failed;
            continue;
        }
        QElapsedTimer timer;
        timer.start();
        const auto res =
            export_synced_video(job.source, rows, fps, job.out, [&](int done, int total) {
                say(QString("  %1%  (%2/%3)  %4s elapsed")
                        .arg(100.0 * done / total, 5, 'f', 1)
                        .arg(done)
                        .arg(total)
                        .arg(timer.elapsed() / 1000.0, 0, 'f', 1));
                return true;
            });
        if (!res.ok) {
            complain(QString("Camera %1: %2").arg(job.camera + 1).arg(res.error));
            ++failed;
            continue;
        }
        say(QString("%1 Done. Camera %2: %3 frame(s), %4 tagged MISSING, %5, %6 MB in %7 s -> %8")
                .arg(kTag)
                .arg(job.camera + 1)
                .arg(res.frames)
                .arg(res.missing)
                .arg(res.encoder)
                .arg(QFileInfo(job.out).size() / 1e6, 0, 'f', 1)
                .arg(timer.elapsed() / 1000.0, 0, 'f', 1)
                .arg(QDir(session).relativeFilePath(job.out)));
    }
    say(QString("  100.0%  (%1/%1)").arg(jobs.size()));
    return failed ? 1 : 0;
}
