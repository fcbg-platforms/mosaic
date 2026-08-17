#include <gtest/gtest.h>

#include <cmath>

#include "video/param_mapping.hpp"

using mosaic::map_normalized_to_int_range;
using mosaic::round_clamp_to_int_range;

TEST(RoundClampToIntRange, RoundsToNearestInteger) {
    EXPECT_EQ(round_clamp_to_int_range(8.4, 0, 63), 8);
    EXPECT_EQ(round_clamp_to_int_range(8.5, 0, 63), 9);
}

TEST(RoundClampToIntRange, ClampsAboveMax) {
    // Exactly the scenario a stale UI value (e.g. a saved blackLevel=64
    // from before the spinbox range was corrected to 0-63) hits at open().
    EXPECT_EQ(round_clamp_to_int_range(64.0, 0, 63), 63);
}

TEST(RoundClampToIntRange, ClampsBelowMin) { EXPECT_EQ(round_clamp_to_int_range(-5.0, 0, 63), 0); }

TEST(MapNormalizedToIntRange, MapsEndpoints) {
    EXPECT_EQ(map_normalized_to_int_range(0.0, 50, 205), 50);
    EXPECT_EQ(map_normalized_to_int_range(1.0, 50, 205), 205);
}

TEST(MapNormalizedToIntRange, MapsMidpoint) {
    EXPECT_EQ(map_normalized_to_int_range(0.5, 50, 205),
              50 + static_cast<int64_t>(std::lround(0.5 * (205 - 50))));
}

TEST(MapNormalizedToIntRange, ClampsOutOfRangeNormalizedInput) {
    // UI spinbox is nominally bounded to [0,1], but the function itself
    // must not trust that — this is exactly what protects a value written
    // via direct settings.json edit from producing an out-of-device-range
    // node write.
    EXPECT_EQ(map_normalized_to_int_range(-0.5, 50, 205), 50);
    EXPECT_EQ(map_normalized_to_int_range(1.5, 50, 205), 205);
}
