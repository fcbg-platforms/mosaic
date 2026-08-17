#pragma once
#include <QVector>
#include <algorithm>
#include <iterator>

namespace mosaic {

/// Binary-search "nearest by key" lookup, shared by every per-frame/per-window
/// analysis result class (PoseAnalysisResult, GazeFusionResult,
/// ExpressionResult, Skeleton3DResult, RppgResult's nearest_window()/
/// nearest_frame()) — previously an identical, independently-duplicated
/// std::lower_bound + before/after-edge-clamp + numerically-closer tie-break
/// implementation in each of those 6 files. `items` must be ascending by
/// `keyOf(item)` (already guaranteed by every caller's own load() step).
/// Returns nullptr for an empty container; otherwise the item whose key is
/// closest to `estimate`, clamping to the first/last item when `estimate`
/// falls outside the container's range, and preferring the earlier item on
/// an exact tie between neighbors.
template <typename T, typename Key, typename KeyOf>
[[nodiscard]] const T* nearest_by_key(const QVector<T>& items, Key estimate, KeyOf keyOf) {
    if (items.isEmpty()) {
        return nullptr;
    }

    const auto it = std::lower_bound(
        items.begin(), items.end(), estimate,
        [&keyOf](const T& item, Key k) { return keyOf(item) < k; });

    if (it == items.begin()) {
        return &(*it);
    }
    if (it == items.end()) {
        return &(*std::prev(it));
    }

    const auto prevIt = std::prev(it);
    const Key afterDelta  = keyOf(*it) - estimate;
    const Key beforeDelta = estimate - keyOf(*prevIt);
    return (beforeDelta <= afterDelta) ? &(*prevIt) : &(*it);
}

} // namespace mosaic
