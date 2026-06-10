#include "utils/ring_buffer.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using mosaic::RingBuffer;

// ── Basic push / pop ───────────────────────────────────────────────────────

TEST(RingBuffer, PushAndPop) {
    RingBuffer<int> buf(8);
    EXPECT_TRUE(buf.push(42));
    int out{};
    EXPECT_TRUE(buf.pop(out));
    EXPECT_EQ(out, 42);
}

TEST(RingBuffer, EmptyPop) {
    RingBuffer<int> buf(4);
    int out{};
    EXPECT_FALSE(buf.pop(out));
}

TEST(RingBuffer, FullPush) {
    RingBuffer<int> buf(4);
    EXPECT_TRUE(buf.push(1));
    EXPECT_TRUE(buf.push(2));
    EXPECT_TRUE(buf.push(3));
    EXPECT_TRUE(buf.push(4));
    // Buffer is full — next push must fail.
    EXPECT_FALSE(buf.push(5));
}

TEST(RingBuffer, OrderPreserved) {
    RingBuffer<int> buf(16);
    for (int i = 0; i < 8; ++i) { EXPECT_TRUE(buf.push(i)); }
    for (int i = 0; i < 8; ++i) {
        int out{};
        EXPECT_TRUE(buf.pop(out));
        EXPECT_EQ(out, i);
    }
}

TEST(RingBuffer, CapacityRoundsUpToPowerOfTwo) {
    RingBuffer<int> buf(5);
    EXPECT_EQ(buf.capacity(), 8U);
}

TEST(RingBuffer, AvailableTracking) {
    RingBuffer<int> buf(8);
    EXPECT_EQ(buf.available_read(),  0U);
    EXPECT_EQ(buf.available_write(), 8U);
    buf.push(1);
    EXPECT_EQ(buf.available_read(),  1U);
    EXPECT_EQ(buf.available_write(), 7U);
    int out{};
    buf.pop(out);
    EXPECT_EQ(buf.available_read(),  0U);
    EXPECT_EQ(buf.available_write(), 8U);
}

TEST(RingBuffer, MoveSemantics) {
    RingBuffer<std::vector<int>> buf(4);
    std::vector<int> vec = {1, 2, 3};
    EXPECT_TRUE(buf.push(std::move(vec)));
    EXPECT_TRUE(vec.empty()); // moved-from
    std::vector<int> out;
    EXPECT_TRUE(buf.pop(out));
    ASSERT_EQ(out.size(), 3U);
    EXPECT_EQ(out[0], 1);
}

// ── Concurrent SPSC ───────────────────────────────────────────────────────

TEST(RingBuffer, ConcurrentSpsc) {
    constexpr int kItems    = 100'000;
    constexpr int kCapacity = 512;

    RingBuffer<int> buf(kCapacity);

    std::thread producer([&] {
        for (int i = 0; i < kItems; ++i) {
            while (!buf.push(i)) { /* spin */ }
        }
    });

    std::thread consumer([&] {
        for (int expected = 0; expected < kItems; ++expected) {
            int val{};
            while (!buf.pop(val)) { /* spin */ }
            EXPECT_EQ(val, expected);
        }
    });

    producer.join();
    consumer.join();
}

TEST(RingBuffer, ResetClearsPrevious) {
    RingBuffer<int> buf(4);
    buf.push(99);
    buf.reset(8);
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.capacity(), 8U);
}
