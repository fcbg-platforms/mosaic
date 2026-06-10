#include "utils/timestamp.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <chrono>

// ── elapsed_ns ─────────────────────────────────────────────────────────────

TEST(Timestamp, ElapsedNsIsNonNegative) {
    EXPECT_GE(mosaic::elapsed_ns(), 0LL);
}

TEST(Timestamp, ElapsedNsGrowsMonotonically) {
    const int64_t t1 = mosaic::elapsed_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const int64_t t2 = mosaic::elapsed_ns();
    EXPECT_GT(t2, t1);
}

TEST(Timestamp, ElapsedMsMatchesNs) {
    const int64_t ns = mosaic::elapsed_ns();
    const int64_t ms = mosaic::elapsed_ms();
    // elapsed_ms() = elapsed_ns() / 1'000'000 — they're computed independently
    // so allow 2 ms of jitter.
    EXPECT_NEAR(ns / 1'000'000LL, ms, 2LL);
}

TEST(Timestamp, ElapsedSMatchesNs) {
    const int64_t ns = mosaic::elapsed_ns();
    const double   s = mosaic::elapsed_s();
    EXPECT_NEAR(static_cast<double>(ns) / 1e9, s, 0.002);
}

// ── wall_clock_ns ──────────────────────────────────────────────────────────

TEST(Timestamp, WallClockNsIsPositive) {
    EXPECT_GT(mosaic::wall_clock_ns(), 0LL);
}

TEST(Timestamp, WallClockNsGrowsMonotonically) {
    const int64_t w1 = mosaic::wall_clock_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const int64_t w2 = mosaic::wall_clock_ns();
    EXPECT_GT(w2, w1);
}

// ── wall_clock_string ──────────────────────────────────────────────────────

TEST(Timestamp, WallClockStringNotEmpty) {
    EXPECT_FALSE(mosaic::wall_clock_string().isEmpty());
}

TEST(Timestamp, WallClockFilenameFormat) {
    // Expected pattern: "YYYY-MM-DD_HH-MM-SS" — 19 characters.
    const QString name = mosaic::wall_clock_filename();
    EXPECT_EQ(name.length(), 19);
    EXPECT_EQ(name[4],  '-');
    EXPECT_EQ(name[7],  '-');
    EXPECT_EQ(name[10], '_');
    EXPECT_EQ(name[13], '-');
    EXPECT_EQ(name[16], '-');
}

// ── monotonicity across threads ────────────────────────────────────────────

TEST(Timestamp, ElapsedNsThreadSafe) {
    constexpr int kThreads = 4;
    constexpr int kSamples = 10'000;

    std::vector<std::vector<int64_t>> results(kThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t, &results] {
            results[t].reserve(kSamples);
            for (int i = 0; i < kSamples; ++i) {
                results[t].push_back(mosaic::elapsed_ns());
            }
        });
    }
    for (auto& thr : threads) { thr.join(); }

    for (int t = 0; t < kThreads; ++t) {
        for (int i = 1; i < kSamples; ++i) {
            EXPECT_GE(results[t][i], results[t][i-1])
                << "Thread " << t << " sample " << i << " went backwards.";
        }
    }
}
