#include "ui/audio/speaker_palette.hpp"

namespace mosaic {

QMap<QString, int> assign_speaker_palette_indices(const QStringList& orderedLabels) {
    QMap<QString, int> result;
    int next = 0;
    for (const QString& label : orderedLabels) {
        if (label.isEmpty() || result.contains(label)) { continue; }
        result.insert(label, next++);
    }
    return result;
}

} // namespace mosaic
