#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cassert>
#include <new>
#include <array>
#include <optional>

namespace micro_exchange::md {

/**
 * SPSCRingBuffer — Single-Producer Single-Consumer lock-free ring buffer.
 *
 * Design rationale:
 * ─────────────────
 * The market data pipeline has a natural producer-consumer topology:
 *
 *   [Matching Engine Thread] → buffer → [Feed Publisher Thread]
 *
 * An SPSC ring buffer is the optimal primitive here because:
 *
 *   1. No locks: producer and consumer never contend
 *   2. No CAS loops: only relaxed/acquire/release atomics needed
 *   3. Bounded memory: no dynamic allocation after construction
 *   4. Cache-friendly: sequential access pattern
 *   5. Wait-free: both push and pop complete in bounded steps
 *
 * The classic Lamport formulation with two cache-line-separated indices:
 *   • write_pos_: only modified by producer, read by consumer
 *   • read_pos_:  only modified by consumer, read by producer
 *
 * False sharing prevention: positions are on separate cache lines.
 *
 * Capacity must be a power of 2 for efficient modular arithmetic (mask).
 */
template <typename T, size_t Capacity>
class SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
        "Capacity must be a power of 2");
    static_assert(Capacity > 0, "Capacity must be positive");

    static constexpr size_t MASK = Capacity - 1;

public:
    SPSCRingBuffer() = default;

    /**
     * Push an element (producer only).
     * Returns false if buffer is full (back-pressure signal).
     */
    bool push(const T& item) noexcept {
        const size_t write = write_pos_.load(std::memory_order_relaxed);
        const size_t next  = (write + 1) & MASK;

        if (next == read_pos_.load(std::memory_order_acquire)) {
            return false;  // Full — apply back-pressure
        }

        buffer_[write] = item;
        write_pos_.store(next, std::memory_order_release);
        return true;
    }

    /**
     * Pop an element (consumer only).
     * Returns nullopt if buffer is empty.
     */
    std::optional<T> pop() noexcept {
        const size_t read = read_pos_.load(std::memory_order_relaxed);

        if (read == write_pos_.load(std::memory_order_acquire)) {
            return std::nullopt;  // Empty
        }

        T item = buffer_[read];
        read_pos_.store((read + 1) & MASK, std::memory_order_release);
        return item;
    }

    /**
     * Peek without consuming (consumer only).
     */
    [[nodiscard]] std::optional<T> peek() const noexcept {
        const size_t read = read_pos_.load(std::memory_order_relaxed);

        if (read == write_pos_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }

        return buffer_[read];
    }

    [[nodiscard]] bool empty() const noexcept {
        return read_pos_.load(std::memory_order_acquire)
            == write_pos_.load(std::memory_order_acquire);
    }

    [[nodiscard]] size_t size() const noexcept {
        const size_t w = write_pos_.load(std::memory_order_acquire);
        const size_t r = read_pos_.load(std::memory_order_acquire);
        return (w - r) & MASK;
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Capacity - 1;  // One slot reserved for full/empty disambiguation
    }

private:
    // Separate cache lines to prevent false sharing
    alignas(64) std::atomic<size_t>  write_pos_{0};
    alignas(64) std::atomic<size_t>  read_pos_{0};
    alignas(64) std::array<T, Capacity> buffer_{};
};

} // namespace micro_exchange::md
