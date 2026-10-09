#include <gtest/gtest.h>

#include "video/gige_bandwidth.hpp"

using mosaic::bytes_per_pixel;
using mosaic::gige_link_utilisation;
using mosaic::k_gige_line_rate_bytes_per_sec;
using mosaic::max_fps_for_link;
using mosaic::required_bytes_per_second;
using mosaic::same_pixel_format;

// ── bytes_per_pixel ────────────────────────────────────────────────────────

// BGR8 is interpolated in the camera and costs three bytes per pixel; Bayer is
// the raw sensor mosaic at one, interpolated on the host.
TEST(GigeBandwidth, ColourFormatsCostThreeBytesAndBayerCostsOne) {
    EXPECT_EQ(bytes_per_pixel("BGR8"), 3.0);
    EXPECT_EQ(bytes_per_pixel("RGB8"), 3.0);
    EXPECT_EQ(bytes_per_pixel("BayerRG8"), 1.0);
    EXPECT_EQ(bytes_per_pixel("BayerGB8"), 1.0);
    EXPECT_EQ(bytes_per_pixel("Mono8"), 1.0);
    EXPECT_EQ(bytes_per_pixel("Mono12"), 2.0);
}

// VideoGrabber falls back to the SFNC 1.x spellings, and a colour GigE camera
// that accepts none of the requested formats often stays in YUV 4:2:2. Missing
// any of these would report a real stream as unmeasurable.
TEST(GigeBandwidth, FallbackAndYuvSpellingsAreRecognised) {
    EXPECT_EQ(bytes_per_pixel("BGR8Packed"), 3.0);
    EXPECT_EQ(bytes_per_pixel("RGB8Packed"), 3.0);
    EXPECT_EQ(bytes_per_pixel("YUV422Packed"), 2.0);
    EXPECT_EQ(bytes_per_pixel("YUV422_YUYV_Packed"), 2.0);
    EXPECT_EQ(bytes_per_pixel("YCbCr422_8"), 2.0);
}

// Deeper formats: two bytes unpacked, one and a half packed, not rounded.
TEST(GigeBandwidth, TwelveBitFormatsCostTwoBytesOrOneAndAHalfPacked) {
    EXPECT_EQ(bytes_per_pixel("BayerGB12"), 2.0);
    EXPECT_EQ(bytes_per_pixel("Mono16"), 2.0);
    EXPECT_EQ(bytes_per_pixel("BayerGB12Packed"), 1.5);
    EXPECT_EQ(bytes_per_pixel("Mono12Packed"), 1.5);
    EXPECT_EQ(bytes_per_pixel("YUV411Packed"), 1.5);
    EXPECT_EQ(bytes_per_pixel("YUV444Packed"), 3.0);
    EXPECT_NEAR(required_bytes_per_second(1920, 1080, 25.0, "BayerGB12Packed") / 1e6, 77.76, 0.01);
}

// Unknown means unknown. A guess here would look authoritative and be wrong.
TEST(GigeBandwidth, UnknownFormatsReportZeroRatherThanGuessing) {
    EXPECT_EQ(bytes_per_pixel(""), 0.0);
    EXPECT_EQ(bytes_per_pixel("bgr8"), 0.0); // GenICam names are case-sensitive
    EXPECT_EQ(bytes_per_pixel("Nonsense"), 0.0);
}

// A request accepted under its SFNC 1.x spelling was not rejected.
TEST(GigeBandwidth, PackedSpellingsOfOneFormatAreTheSameFormat) {
    EXPECT_TRUE(same_pixel_format("BGR8", "BGR8"));
    EXPECT_TRUE(same_pixel_format("BGR8", "BGR8Packed"));
    EXPECT_TRUE(same_pixel_format("RGB8Packed", "RGB8"));
    EXPECT_FALSE(same_pixel_format("BGR8", "YUV422Packed"));
    EXPECT_FALSE(same_pixel_format("BGR8", "RGB8Packed"));
    EXPECT_FALSE(same_pixel_format("BayerRG8", "BayerGB8"));
}

// ── The format moves the answer across the limit ───────────────────────────

// Why the check must use the format the camera reports, not the one asked for:
// at 1920x1080 and 25 fps BGR8 cannot fit, YUV 4:2:2 fits with little room,
// and Bayer fits easily.
TEST(GigeBandwidth, AtFullResolutionTheFormatDecidesWhetherItFits) {
    EXPECT_DOUBLE_EQ(required_bytes_per_second(1920, 1080, 25.0, "BGR8"),
                     1920.0 * 1080 * 3 * 25); // 155.52 MB/s
    EXPECT_NEAR(gige_link_utilisation(1920, 1080, 25.0, "BGR8"), 1.244, 0.001);

    EXPECT_NEAR(required_bytes_per_second(1920, 1080, 25.0, "YUV422Packed") / 1e6, 103.68, 0.01);
    EXPECT_NEAR(gige_link_utilisation(1920, 1080, 25.0, "YUV422Packed"), 0.829, 0.001);

    // The "~52 MB/s per camera" scripts/setup_nic_cameras.ps1 was sized for.
    EXPECT_NEAR(required_bytes_per_second(1920, 1080, 25.0, "BayerRG8") / 1e6, 51.84, 0.01);
}

// ── max_fps_for_link ───────────────────────────────────────────────────────

TEST(GigeBandwidth, ReportsTheCeilingForEachFormat) {
    EXPECT_NEAR(max_fps_for_link(1920, 1080, "BGR8"), 20.1, 0.1);
    EXPECT_NEAR(max_fps_for_link(1920, 1080, "YUV422Packed"), 30.1, 0.1);
    EXPECT_GT(max_fps_for_link(1920, 1080, "BayerRG8"), 60.0);
    // Interview mode's crop: a smaller frame raises the ceiling.
    EXPECT_GT(max_fps_for_link(1280, 540, "BGR8"), 60.0);
}

TEST(GigeBandwidth, ACeilingIsConsistentWithItsOwnUtilisation) {
    const double ceiling = max_fps_for_link(1920, 1080, "BGR8");
    EXPECT_NEAR(gige_link_utilisation(1920, 1080, ceiling, "BGR8"), 1.0, 1e-9);
    EXPECT_NEAR(required_bytes_per_second(1920, 1080, ceiling, "BGR8"),
                k_gige_line_rate_bytes_per_sec, 1e-3);
}

// ── Degenerate inputs ──────────────────────────────────────────────────────

TEST(GigeBandwidth, NonsenseInputsReportZeroNotInfinityOrNaN) {
    EXPECT_DOUBLE_EQ(required_bytes_per_second(0, 1080, 25.0, "BGR8"), 0.0);
    EXPECT_DOUBLE_EQ(required_bytes_per_second(1920, 0, 25.0, "BGR8"), 0.0);
    EXPECT_DOUBLE_EQ(required_bytes_per_second(1920, 1080, 0.0, "BGR8"), 0.0);
    EXPECT_DOUBLE_EQ(required_bytes_per_second(1920, 1080, -5.0, "BGR8"), 0.0);
    EXPECT_DOUBLE_EQ(required_bytes_per_second(1920, 1080, 25.0, "Nonsense"), 0.0);
    EXPECT_DOUBLE_EQ(max_fps_for_link(0, 0, "BGR8"), 0.0);
    EXPECT_DOUBLE_EQ(max_fps_for_link(1920, 1080, "Nonsense"), 0.0);
    // An unknown format reads as "cannot judge", never as "fits comfortably".
    EXPECT_DOUBLE_EQ(gige_link_utilisation(1920, 1080, 25.0, "Nonsense"), 0.0);
}
