#pragma once
#include <atomic>
#include <cstddef>
#include <vector>

namespace mosaic {

/// @brief Lock-free Single-Producer / Single-Consumer (SPSC) ring buffer.
///
/// Designed for the video pipeline: one VideoGrabber thread pushes frames,
/// one VideoEncoder thread pops them — with zero locks and zero dynamic
/// allocation after construction.
///
/// @par Usage rules
/// - push() must only ever be called from **one thread** (the producer).
/// - pop()  must only ever be called from **one thread** (the consumer).
/// - The two atomics are on separate cache lines to prevent false sharing.
/// - Capacity is rounded up to the next power of two internally.
///
/// @par Example
/// @code{.cpp}
/// RingBuffer<std::shared_ptr<VideoFrame>> buf(128);
///
/// // Producer thread:
/// if (!buf.push(frame)) {
///     log_warning("Ring buffer full — frame dropped");
/// }
///
/// // Consumer thread:
/// std::shared_ptr<VideoFrame> f;
/// while (buf.pop(f)) {
///     encode(f);
/// }
/// @endcode
///
/// @tparam T  Element type.  Must be default-constructible.
template <typename T>
class RingBuffer {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // structure padded due to alignas — intentional
#endif
    struct alignas(64) AlignedAtomic {
        std::atomic<std::size_t> v{0};
    };
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    AlignedAtomic m_head; ///< Producer counter (written by producer, read by consumer).
    AlignedAtomic m_tail; ///< Consumer counter (written by consumer, read by producer).
    std::size_t m_capacity{0};
    std::size_t m_mask{0};
    std::vector<T> m_storage;

   public:
    /// @param capacity  Desired capacity.  Rounded up to the next power of two.
    explicit RingBuffer(std::size_t capacity = 0) { reset(capacity); }

    /// @brief Resizes and clears the buffer.
    ///
    /// @warning Not thread-safe — call only when no producer or consumer thread
    ///          is running.
    ///
    /// @param capacity  New capacity.  Rounded up to the next power of two.
    void reset(std::size_t capacity) {
        std::size_t p2 = 1;
        while (p2 < capacity) p2 <<= 1;
        m_capacity = p2;
        m_mask     = p2 - 1;
        m_storage.assign(p2, T{});
        m_head.v.store(0, std::memory_order_relaxed);
        m_tail.v.store(0, std::memory_order_relaxed);
    }

    /// @brief Writes one item (copy).
    ///
    /// Called from the **producer thread only**.
    ///
    /// @param item  Item to enqueue.
    /// @returns     @c false if the buffer is full (item is discarded).
    [[nodiscard]] bool push(const T& item) noexcept {
        const auto head = m_head.v.load(std::memory_order_relaxed);
        const auto tail = m_tail.v.load(std::memory_order_acquire);
        if (head - tail >= m_capacity) return false;
        m_storage[head & m_mask] = item;
        m_head.v.store(head + 1, std::memory_order_release);
        return true;
    }

    /// @brief Writes one item (move).
    ///
    /// Called from the **producer thread only**.
    ///
    /// @param item  Item to move-enqueue.
    /// @returns     @c false if the buffer is full (item is discarded).
    [[nodiscard]] bool push(T&& item) noexcept {
        const auto head = m_head.v.load(std::memory_order_relaxed);
        const auto tail = m_tail.v.load(std::memory_order_acquire);
        if (head - tail >= m_capacity) return false;
        m_storage[head & m_mask] = std::move(item);
        m_head.v.store(head + 1, std::memory_order_release);
        return true;
    }

    /// @brief Reads and removes one item.
    ///
    /// Called from the **consumer thread only**.
    ///
    /// @param out  Output reference populated with the dequeued item.
    /// @returns    @c false if the buffer is empty.
    [[nodiscard]] bool pop(T& out) noexcept {
        const auto tail = m_tail.v.load(std::memory_order_relaxed);
        const auto head = m_head.v.load(std::memory_order_acquire);
        if (head == tail) return false;
        out = std::move(m_storage[tail & m_mask]);
        m_tail.v.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// @returns Number of items available for reading.
    [[nodiscard]] std::size_t available_read() const noexcept {
        return m_head.v.load(std::memory_order_acquire) - m_tail.v.load(std::memory_order_acquire);
    }

    /// @returns Number of additional items that can be pushed before the buffer is full.
    [[nodiscard]] std::size_t available_write() const noexcept {
        return m_capacity - available_read();
    }

    /// @returns The actual capacity (always a power of two).
    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }

    /// @returns @c true if no items are available for reading.
    [[nodiscard]] bool empty() const noexcept { return available_read() == 0; }

    /// @returns @c true if no more items can be pushed without dropping.
    [[nodiscard]] bool full() const noexcept { return available_read() >= m_capacity; }
};

} // namespace mosaic
