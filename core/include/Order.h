#pragma once

#include <cstdint>
#include <chrono>
#include <string>
#include <cstring>
#include <limits>

namespace micro_exchange::core {

// ─────────────────────────────────────────────
// Enumerations
// ─────────────────────────────────────────────

enum class Side : uint8_t {
    Buy  = 0,
    Sell = 1
};

enum class OrderType : uint8_t {
    Limit  = 0,
    Market = 1,
    IOC    = 2,   // Immediate or Cancel
    FOK    = 3    // Fill or Kill
};

enum class TimeInForce : uint8_t {
    GTC = 0,  // Good till cancel
    IOC = 1,  // Immediate or cancel
    FOK = 2,  // Fill or kill
    DAY = 3   // Day order
};

enum class OrderStatus : uint8_t {
    New             = 0,
    PartiallyFilled = 1,
    Filled          = 2,
    Cancelled       = 3,
    Rejected        = 4,
    Amended         = 5
};

// ─────────────────────────────────────────────
// Price representation
// Fixed-point: price in ticks (integer cents or sub-cents)
// Avoids floating-point in the hot path entirely.
// ─────────────────────────────────────────────

using Price    = int64_t;   // Price in ticks (1 tick = 0.01 USD by default)
using Quantity = uint64_t;
using OrderId  = uint64_t;
using SeqNum   = uint64_t;

static constexpr Price PRICE_INVALID = std::numeric_limits<Price>::max();
static constexpr Price PRICE_MARKET  = 0;  // Market orders have no price limit

// ─────────────────────────────────────────────
// Timestamp: nanosecond precision
// ─────────────────────────────────────────────

using Timestamp = std::chrono::time_point<std::chrono::steady_clock>;

inline uint64_t timestamp_ns(Timestamp ts) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            ts.time_since_epoch()
        ).count()
    );
}

inline Timestamp now() {
    return std::chrono::steady_clock::now();
}

// ─────────────────────────────────────────────
// Order
// Cache-line aligned for performance.
// Intrusive linked-list pointers for O(1) queue ops.
// ─────────────────────────────────────────────

struct alignas(64) Order {
    // ── Identifiers ──
    OrderId     id          = 0;
    SeqNum      sequence    = 0;       // Global sequence for determinism

    // ── Order parameters ──
    Side        side        = Side::Buy;
    OrderType   type        = OrderType::Limit;
    TimeInForce tif         = TimeInForce::GTC;
    Price       price       = 0;       // In ticks
    Quantity    quantity    = 0;        // Original quantity
    Quantity    filled_qty  = 0;        // Cumulative filled
    Quantity    leaves_qty  = 0;        // Remaining = quantity - filled_qty

    // ── Timestamps ──
    Timestamp   entry_time  = {};
    Timestamp   last_update = {};

    // ── Status ──
    OrderStatus status      = OrderStatus::New;

    // ── Intrusive doubly-linked list pointers ──
    // Used by PriceLevel to maintain FIFO queue without heap allocation.
    Order*      prev        = nullptr;
    Order*      next        = nullptr;

    // ── Symbol (for multi-instrument support) ──
    char        symbol[16]  = {};

    // ── Convenience ──
    [[nodiscard]] bool is_buy() const noexcept { return side == Side::Buy; }
    [[nodiscard]] bool is_filled() const noexcept { return leaves_qty == 0; }
    [[nodiscard]] bool is_active() const noexcept {
        return status == OrderStatus::New || status == OrderStatus::PartiallyFilled;
    }

    void fill(Quantity qty) noexcept {
        filled_qty += qty;
        leaves_qty -= qty;
        last_update = now();
        status = (leaves_qty == 0) ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
    }

    void cancel() noexcept {
        status = OrderStatus::Cancelled;
        leaves_qty = 0;
        last_update = now();
    }
};

// ─────────────────────────────────────────────
// Trade (execution report)
// ─────────────────────────────────────────────

struct Trade {
    SeqNum      sequence     = 0;
    OrderId     buy_order_id = 0;
    OrderId     sell_order_id = 0;
    Price       price        = 0;
    Quantity    quantity     = 0;
    Timestamp   exec_time    = {};
    Side        aggressor    = Side::Buy;  // Who crossed the spread

    char        symbol[16]   = {};
};

// ─────────────────────────────────────────────
// Order request messages (input events)
// ─────────────────────────────────────────────

struct NewOrderRequest {
    OrderId     id;
    Side        side;
    OrderType   type;
    TimeInForce tif;
    Price       price;
    Quantity    quantity;
    char        symbol[16];
};

struct CancelRequest {
    OrderId     order_id;
    char        symbol[16];
};

struct AmendRequest {
    OrderId     order_id;
    Price       new_price;     // 0 = no change
    Quantity    new_quantity;   // 0 = no change
    char        symbol[16];
};

} // namespace micro_exchange::core
