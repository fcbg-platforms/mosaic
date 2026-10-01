#include "analysis/camera_timestamp_files.hpp"

#include <algorithm>

namespace mosaic {

namespace {

// Kept as one definition so the glob used to *find* the files and the parser
// used to *name* them can never drift apart — a mismatch would show up as
// files found but silently discarded.
constexpr auto k_prefix = "timestamps_cam";
constexpr auto k_suffix = ".csv";

} // namespace

int parse_camera_timestamp_index(const QString& fileName) {
    const QString prefix(k_prefix);
    const QString suffix(k_suffix);
    if (!fileName.startsWith(prefix) || !fileName.endsWith(suffix)) {
        return -1;
    }
    const qsizetype digitsLen = fileName.size() - prefix.size() - suffix.size();
    if (digitsLen <= 0) {
        return -1; // "timestamps_cam.csv" — no number at all
    }
    const QString digits = fileName.mid(prefix.size(), digitsLen);

    // toInt() alone is not enough: it accepts leading whitespace and a sign, so
    // " 2" and "-2" would both parse. Require pure digits, so the only thing
    // this ever returns is a real camera number.
    for (const QChar c : digits) {
        if (!c.isDigit()) {
            return -1;
        }
    }
    bool ok            = false;
    const int camIndex = digits.toInt(&ok);
    return ok ? camIndex : -1;
}

QVector<int> discover_camera_timestamp_indices(const QDir& videoDir) {
    QVector<int> out;
    const QStringList names = videoDir.entryList({QString(k_prefix) + "*" + k_suffix}, QDir::Files);
    out.reserve(names.size());
    for (const QString& name : names) {
        const int camIndex = parse_camera_timestamp_index(name);
        if (camIndex >= 0) {
            out.append(camIndex);
        }
    }
    // Numeric, not the listing's lexicographic order — "cam10" precedes "cam2"
    // as text, which would report cameras out of order and, worse, make the
    // manifest's own ordering depend on how many cameras a rig happens to have.
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace mosaic
