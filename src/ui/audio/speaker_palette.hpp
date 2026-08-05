#pragma once
#include <QMap>
#include <QStringList>
#include <QString>

namespace mosaic {

// Assigns each distinct non-empty label an index (0, 1, 2, ...) in order of
// first appearance in `orderedLabels` (repeats of an already-seen label
// reuse its existing index). Empty strings are never assigned an index —
// diarization's "no speaker attributed" case. Indices are NOT capped or
// wrapped here; that's the caller's job when mapping an index onto a
// finite color palette (see AudioWaveformW::speaker_color()), keeping this
// function's contract simple and independent of any particular palette
// size.
[[nodiscard]] QMap<QString, int> assign_speaker_palette_indices(const QStringList& orderedLabels);

} // namespace mosaic
