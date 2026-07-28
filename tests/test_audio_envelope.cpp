#include "audio/audio_envelope.hpp"
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

using mosaic::compute_envelope;
using mosaic::compute_rms;
using mosaic::convert_float32_to_int16;
using mosaic::convert_int32_to_int16;

TEST(ComputeEnvelope, EmptyBufferIsZero) {
    const auto env = compute_envelope(nullptr, 0);
    EXPECT_FLOAT_EQ(env.minSample, 0.0f);
    EXPECT_FLOAT_EQ(env.maxSample, 0.0f);
}

TEST(ComputeEnvelope, FindsPositiveAndNegativePeaksIndependently) {
    const std::vector<int16_t> samples = {0, 16384, -8192, 100, -32768, 32767, 0};
    const auto env = compute_envelope(reinterpret_cast<const char*>(samples.data()),
                                       static_cast<qint64>(samples.size() * sizeof(int16_t)));
    EXPECT_NEAR(env.minSample, -1.0f, 1e-4f);
    EXPECT_NEAR(env.maxSample, 32767.0f / 32768.0f, 1e-4f);
}

TEST(ComputeEnvelope, ConstantBufferHasEqualMinAndMax) {
    const std::vector<int16_t> samples(32, 1000);
    const auto env = compute_envelope(reinterpret_cast<const char*>(samples.data()),
                                       static_cast<qint64>(samples.size() * sizeof(int16_t)));
    EXPECT_NEAR(env.minSample, env.maxSample, 1e-6f);
    EXPECT_NEAR(env.minSample, 1000.0f / 32768.0f, 1e-4f);
}

TEST(ComputeEnvelope, MinNeverExceedsMax) {
    // A single-sample buffer must still produce min <= max (both equal to
    // that sample) — regression check for the "seed from s[0], never from
    // 0" invariant that makes a purely-negative or purely-positive buffer
    // report a correct, non-degenerate envelope.
    const std::vector<int16_t> samples = {-500};
    const auto env = compute_envelope(reinterpret_cast<const char*>(samples.data()),
                                       static_cast<qint64>(samples.size() * sizeof(int16_t)));
    EXPECT_LE(env.minSample, env.maxSample);
    EXPECT_NEAR(env.minSample, -500.0f / 32768.0f, 1e-4f);
    EXPECT_NEAR(env.maxSample, -500.0f / 32768.0f, 1e-4f);
}

TEST(ComputeRms, EmptyBufferIsZero) {
    EXPECT_FLOAT_EQ(compute_rms(nullptr, 0), 0.0f);
}

TEST(ComputeRms, SilenceIsZero) {
    const std::vector<int16_t> samples(64, 0);
    EXPECT_FLOAT_EQ(compute_rms(reinterpret_cast<const char*>(samples.data()),
                                 static_cast<qint64>(samples.size() * sizeof(int16_t))),
                    0.0f);
}

TEST(ComputeRms, FullScaleConstantSignalIsOne) {
    // -32768 is exactly representable in int16_t and divides evenly by
    // 32768.0, giving an exact 1.0 RMS — 32767 (the positive max) doesn't
    // divide evenly and would only approximate 1.0.
    const std::vector<int16_t> samples(16, -32768);
    EXPECT_NEAR(compute_rms(reinterpret_cast<const char*>(samples.data()),
                             static_cast<qint64>(samples.size() * sizeof(int16_t))),
                1.0f, 1e-6f);
}

// Regression tests for a real bug: some audio interfaces (confirmed on real
// hardware — a RØDE NT-USB Mini) don't support 16-bit PCM capture at all,
// only 32-bit float or 32-bit int. AudioRecorder now captures in whichever
// format the device actually offers and converts to 16-bit PCM via these
// functions before anything else (WavWriter, compute_rms, compute_envelope)
// ever sees the buffer.

TEST(ConvertFloat32ToInt16, EmptyBufferProducesEmptyOutput) {
    const auto out = convert_float32_to_int16(nullptr, 0);
    EXPECT_TRUE(out.isEmpty());
}

TEST(ConvertFloat32ToInt16, RoundTripsKnownValues) {
    const std::vector<float> samples = {0.0f, 1.0f, -1.0f, 0.5f, -0.5f};
    const auto out = convert_float32_to_int16(reinterpret_cast<const char*>(samples.data()),
                                               static_cast<qint64>(samples.size() * sizeof(float)));
    ASSERT_EQ(out.size(), static_cast<qsizetype>(samples.size() * sizeof(int16_t)));
    const auto* result = reinterpret_cast<const int16_t*>(out.constData());
    EXPECT_EQ(result[0], 0);
    EXPECT_EQ(result[1], 32767);
    EXPECT_EQ(result[2], -32767);
    EXPECT_NEAR(result[3], 16384, 1);
    EXPECT_NEAR(result[4], -16384, 1);
}

TEST(ConvertFloat32ToInt16, ClampsOutOfRangeValues) {
    // A misbehaving/clipping source could exceed [-1, 1] — must not wrap
    // around int16 instead of saturating.
    const std::vector<float> samples = {2.0f, -2.0f};
    const auto out = convert_float32_to_int16(reinterpret_cast<const char*>(samples.data()),
                                               static_cast<qint64>(samples.size() * sizeof(float)));
    const auto* result = reinterpret_cast<const int16_t*>(out.constData());
    EXPECT_EQ(result[0], 32767);
    EXPECT_EQ(result[1], -32767);
}

TEST(ConvertInt32ToInt16, EmptyBufferProducesEmptyOutput) {
    const auto out = convert_int32_to_int16(nullptr, 0);
    EXPECT_TRUE(out.isEmpty());
}

TEST(ConvertInt32ToInt16, KeepsMostSignificantBits) {
    const std::vector<int32_t> samples = {0, 1 << 30, -(1 << 30)};
    const auto out = convert_int32_to_int16(reinterpret_cast<const char*>(samples.data()),
                                             static_cast<qint64>(samples.size() * sizeof(int32_t)));
    ASSERT_EQ(out.size(), static_cast<qsizetype>(samples.size() * sizeof(int16_t)));
    const auto* result = reinterpret_cast<const int16_t*>(out.constData());
    EXPECT_EQ(result[0], 0);
    EXPECT_EQ(result[1], (1 << 30) >> 16);
    EXPECT_EQ(result[2], -(1 << 30) >> 16);
}
