#pragma once

#include "Order.h"
#include "PriceLevel.h"
#include "ArenaAllocator.h"

#include <vector>
#include <unordered_map>
#include <functional>
#include <optional>
#include <string>
#include <cstring>
#include <cstdint>
#include <algorithm>

namespace micro_exchange::core {

/**
 * ArrayOrderBook — CLOB backed by a contiguous, tick-indexed array of
 * PriceLevels plus a bitmap occupied-index (the layout production matching
 * engines such as LMAX use), as opposed to `OrderBook`'s `std::map`.
 *
 * Why an array AND a bitmap
 * ─────────────────────────
 * A flat array indexed by `(price - min_price)` gives O(1) level lookup,
 * insert, and erase, and lays the levels out sequentially for cache-friendly
 * access. But the best-bid / best-ask cursors still have to *find the next
 * occupied level* when the top of book is consumed. A naive linear scan makes
 * that O(band): on a wide/sparse book it is catastrophic — measured ~25x
 * slower than `std::map`, because the scan walks thousands of empty slots.
 *
 * The fix is a bitmap over levels (one bit = "this level has resting orders")
 * plus hardware bit-scan: `__builtin_ctzll` to find the next occupied level
 * above a cursor, `__builtin_clzll` to find the next one below. That turns the
 * cursor advance into a word-skipping scan (64 levels per instruction) instead
 * of one-level-at-a-time, making the array robust across band widths.
 *
 * This class is a drop-in alternative to `OrderBook` for the matching hot path
 * and reuses the SAME Order, PriceLevel, and ArenaAllocator, so a head-to-head
 * benchmark isolates exactly one variable: the level container. See
 * `bench/bench_orderbook_compare.cpp`, which cross-checks that both books emit
 * an identical trade stream and then compares throughput/latency.
 *
 * Scope: Limit / Market / IOC / FOK + cancel (the types the simulator and
 * benchmarks use). Stop / StopLimit / amend live in the full `OrderBook`.
 *
 * Matching semantics are identical to `OrderBook`: price-time priority, FIFO
 * within a level, trade prints at the resting order's price, Limit remainder
 * rests while Market / IOC / unfilled-FOK remainder cancels.
 */
class ArrayOrderBook {
public:
    using TradeCallback = std::function<void(const Trade&)>;
    using OrderCallback = std::function<void(const Order&)>;

    /**
     * @param symbol     instrument symbol
     * @param min_price  lowest supported limit price (ticks), inclusive
     * @param max_price  highest supported limit price (ticks), inclusive
     */
    ArrayOrderBook(const std::string& symbol, Price min_price, Price max_price)
        : symbol_(symbol)
        , min_price_(min_price)
        , max_price_(max_price)
        , order_arena_(65536)
    {
        const size_t n = static_cast<size_t>(max_price_ - min_price_ + 1);
        levels_.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            levels_.emplace_back(min_price_ + static_cast<Price>(i));
        }
        occ_.assign((n + 63) / 64, 0ULL);     // occupied-level bitmap, all empty
        best_bid_idx_ = -1;                    // no bids
        best_ask_idx_ = static_cast<long>(n);  // no asks (one past the end)
    }

    // ── Multi-subscriber dispatch (mirrors OrderBook's API) ──
    void add_trade_listener(TradeCallback cb) { trade_listeners_.push_back(std::move(cb)); }
    void add_order_listener(OrderCallback cb) { order_listeners_.push_back(std::move(cb)); }
    void set_trade_callback(TradeCallback cb) { add_trade_listener(std::move(cb)); }
    void set_order_callback(OrderCallback cb) { add_order_listener(std::move(cb)); }
    void clear_listeners() { trade_listeners_.clear(); order_listeners_.clear(); }

    // ═══════════════════════════════════════════
    // Order operations
    // ═══════════════════════════════════════════

    Order* add_order(const NewOrderRequest& req) {
        Order* order = order_arena_.allocate();
        new (order) Order{};

        order->id         = req.id;
        order->sequence   = next_sequence_++;
        order->side       = req.side;
        order->type       = req.type;
        order->tif        = req.tif;
        order->price      = req.price;
        order->stop_price = req.stop_price;
        order->quantity   = req.quantity;
        order->leaves_qty = req.quantity;
        order->filled_qty = 0;
        order->entry_time = now();
        order->last_update = order->entry_time;
        order->status     = OrderStatus::New;
        std::memcpy(order->symbol, req.symbol, sizeof(order->symbol));

        order_index_[order->id] = order;

        // FOK: only execute if the whole quantity can be filled right now.
        if (order->type == OrderType::FOK && !can_fill_completely(order)) {
            order->cancel();
            order_index_.erase(order->id);
            notify_order(*order);
            return order;
        }

        match(order);

        if (order->leaves_qty > 0) {
            if (order->type == OrderType::Limit && in_band(order->price)) {
                rest_order(order);
            } else {
                // Market / IOC / FOK remainder, or out-of-band limit, cancels.
                order->cancel();
                order_index_.erase(order->id);
                notify_order(*order);
            }
        }
        return order;
    }

    bool cancel_order(OrderId id) {
        auto it = order_index_.find(id);
        if (it == order_index_.end()) return false;

        Order* order = it->second;
        if (!order->is_active()) return false;

        remove_from_book(order);
        order->cancel();
        order_index_.erase(it);
        notify_order(*order);
        return true;
    }

    // ═══════════════════════════════════════════
    // Book state queries
    // ═══════════════════════════════════════════

    [[nodiscard]] std::optional<Price> best_bid() const {
        if (best_bid_idx_ < 0) return std::nullopt;
        return price_at(static_cast<size_t>(best_bid_idx_));
    }

    [[nodiscard]] std::optional<Price> best_ask() const {
        if (best_ask_idx_ >= static_cast<long>(levels_.size())) return std::nullopt;
        return price_at(static_cast<size_t>(best_ask_idx_));
    }

    [[nodiscard]] std::optional<Price> midprice() const {
        auto bb = best_bid();
        auto ba = best_ask();
        if (!bb || !ba) return std::nullopt;
        return (*bb + *ba) / 2;
    }

    [[nodiscard]] std::optional<Price> spread() const {
        auto bb = best_bid();
        auto ba = best_ask();
        if (!bb || !ba) return std::nullopt;
        return *ba - *bb;
    }

    // ── Statistics ──
    [[nodiscard]] uint64_t trade_count()      const { return trade_count_; }
    [[nodiscard]] uint64_t total_volume()     const { return total_volume_; }
    [[nodiscard]] SeqNum   sequence()         const { return next_sequence_; }
    [[nodiscard]] size_t   active_orders()    const { return order_index_.size(); }
    [[nodiscard]] const std::string& symbol() const { return symbol_; }
    [[nodiscard]] Price    last_trade_price() const { return last_trade_price_; }

    [[nodiscard]] bool check_no_crossed_book() const {
        auto bb = best_bid();
        auto ba = best_ask();
        if (!bb || !ba) return true;
        return *bb < *ba;
    }

private:
    // ── Index helpers ──
    [[nodiscard]] size_t idx(Price p)      const { return static_cast<size_t>(p - min_price_); }
    [[nodiscard]] Price  price_at(size_t i) const { return min_price_ + static_cast<Price>(i); }
    [[nodiscard]] bool   in_band(Price p)   const { return p >= min_price_ && p <= max_price_; }

    // ── Occupied-level bitmap ──
    void set_occ(size_t i) { occ_[i >> 6] |=  (1ULL << (i & 63)); }
    void clr_occ(size_t i) { occ_[i >> 6] &= ~(1ULL << (i & 63)); }

    // Lowest occupied level index >= i, or levels_.size() if none.
    [[nodiscard]] long next_occ_ge(long i) const {
        const long n = static_cast<long>(levels_.size());
        if (i < 0) i = 0;
        if (i >= n) return n;
        size_t w = static_cast<size_t>(i) >> 6;
        int b = i & 63;
        uint64_t word = occ_[w] & (~0ULL << b);     // mask off bits below b
        if (word) return static_cast<long>(w << 6) + __builtin_ctzll(word);
        for (size_t k = w + 1; k < occ_.size(); ++k) {
            if (occ_[k]) return static_cast<long>(k << 6) + __builtin_ctzll(occ_[k]);
        }
        return n;
    }

    // Highest occupied level index <= i, or -1 if none.
    [[nodiscard]] long next_occ_le(long i) const {
        const long n = static_cast<long>(levels_.size());
        if (i < 0) return -1;
        if (i >= n) i = n - 1;
        size_t w = static_cast<size_t>(i) >> 6;
        int b = i & 63;
        uint64_t mask = (b == 63) ? ~0ULL : ((1ULL << (b + 1)) - 1);  // bits 0..b
        uint64_t word = occ_[w] & mask;
        if (word) return static_cast<long>(w << 6) + (63 - __builtin_clzll(word));
        for (long k = static_cast<long>(w) - 1; k >= 0; --k) {
            if (occ_[static_cast<size_t>(k)])
                return (k << 6) + (63 - __builtin_clzll(occ_[static_cast<size_t>(k)]));
        }
        return -1;
    }

    // ── Matching ──
    void match(Order* incoming) {
        if (incoming->is_buy()) match_buy(incoming);
        else                    match_sell(incoming);
    }

    void match_buy(Order* incoming) {
        const bool is_market =
            incoming->type == OrderType::Market || incoming->price == PRICE_MARKET;
        long limit_idx = is_market ? static_cast<long>(levels_.size()) - 1
                                   : static_cast<long>(idx(std::min(incoming->price, max_price_)));

        const long n = static_cast<long>(levels_.size());
        while (incoming->leaves_qty > 0 && best_ask_idx_ < n && best_ask_idx_ <= limit_idx) {
            PriceLevel& level = levels_[static_cast<size_t>(best_ask_idx_)];
            fill_against_level(incoming, level);
            if (level.empty()) {
                clr_occ(static_cast<size_t>(best_ask_idx_));
                best_ask_idx_ = next_occ_ge(best_ask_idx_ + 1);   // sweep to next ask
            }
            // Otherwise the incoming order is exhausted; the loop condition ends it.
        }
    }

    void match_sell(Order* incoming) {
        const bool is_market =
            incoming->type == OrderType::Market || incoming->price == PRICE_MARKET;
        long limit_idx = is_market ? 0
                                   : static_cast<long>(idx(std::max(incoming->price, min_price_)));

        while (incoming->leaves_qty > 0 && best_bid_idx_ >= 0 && best_bid_idx_ >= limit_idx) {
            PriceLevel& level = levels_[static_cast<size_t>(best_bid_idx_)];
            fill_against_level(incoming, level);
            if (level.empty()) {
                clr_occ(static_cast<size_t>(best_bid_idx_));
                best_bid_idx_ = next_occ_le(best_bid_idx_ - 1);   // sweep to next bid
            }
        }
    }

    // Fill `incoming` against the FIFO queue at `level` until one side empties.
    // Byte-for-byte the same trade construction as OrderBook::match_against, so
    // both books emit identical trade streams.
    void fill_against_level(Order* incoming, PriceLevel& level) {
        while (incoming->leaves_qty > 0 && !level.empty()) {
            Order* resting = level.front();
            Quantity fill_qty = std::min(incoming->leaves_qty, resting->leaves_qty);

            Trade trade{};
            trade.sequence  = next_sequence_++;
            trade.price     = resting->price;   // price improvement to the resting side
            trade.quantity  = fill_qty;
            trade.exec_time = now();
            trade.aggressor = incoming->side;
            std::memcpy(trade.symbol, incoming->symbol, sizeof(trade.symbol));

            if (incoming->is_buy()) {
                trade.buy_order_id  = incoming->id;
                trade.sell_order_id = resting->id;
            } else {
                trade.buy_order_id  = resting->id;
                trade.sell_order_id = incoming->id;
            }

            level.reduce_quantity(fill_qty);
            incoming->fill(fill_qty);
            resting->fill(fill_qty);

            notify_trade(trade);
            notify_order(*resting);

            ++trade_count_;
            total_volume_ += fill_qty;
            last_trade_price_ = trade.price;

            if (resting->is_filled()) {
                level.pop_front();
                order_index_.erase(resting->id);
            }
        }
    }

    bool can_fill_completely(const Order* order) const {
        Quantity needed = order->leaves_qty;
        const bool is_market =
            order->type == OrderType::Market || order->price == PRICE_MARKET;

        if (order->is_buy()) {
            long limit_idx = is_market ? static_cast<long>(levels_.size()) - 1
                                       : static_cast<long>(idx(std::min(order->price, max_price_)));
            const long n = static_cast<long>(levels_.size());
            for (long i = best_ask_idx_; i < n && i <= limit_idx; ++i) {
                needed -= std::min(needed, levels_[static_cast<size_t>(i)].total_quantity());
                if (needed == 0) return true;
            }
        } else {
            long limit_idx = is_market ? 0
                                       : static_cast<long>(idx(std::max(order->price, min_price_)));
            for (long i = best_bid_idx_; i >= 0 && i >= limit_idx; --i) {
                needed -= std::min(needed, levels_[static_cast<size_t>(i)].total_quantity());
                if (needed == 0) return true;
            }
        }
        return needed == 0;
    }

    // ── Book management ──
    void rest_order(Order* order) {
        const size_t i = idx(order->price);
        const bool was_empty = levels_[i].empty();
        levels_[i].push_back(order);
        if (was_empty) set_occ(i);
        if (order->is_buy()) {
            if (static_cast<long>(i) > best_bid_idx_) best_bid_idx_ = static_cast<long>(i);
        } else {
            if (static_cast<long>(i) < best_ask_idx_) best_ask_idx_ = static_cast<long>(i);
        }
    }

    void remove_from_book(Order* order) {
        if (!in_band(order->price)) return;
        const size_t i = idx(order->price);
        levels_[i].remove(order);
        if (levels_[i].empty()) {
            clr_occ(i);
            if (order->is_buy() && static_cast<long>(i) == best_bid_idx_) {
                best_bid_idx_ = next_occ_le(best_bid_idx_ - 1);
            } else if (!order->is_buy() && static_cast<long>(i) == best_ask_idx_) {
                best_ask_idx_ = next_occ_ge(best_ask_idx_ + 1);
            }
        }
    }

    // ── Notifications ──
    void notify_trade(const Trade& t) { for (auto& cb : trade_listeners_) cb(t); }
    void notify_order(const Order& o) { for (auto& cb : order_listeners_) cb(o); }

    // ── Members ──
    std::string             symbol_;
    Price                   min_price_;
    Price                   max_price_;
    std::vector<PriceLevel> levels_;          // contiguous, indexed by price - min_price_
    std::vector<uint64_t>   occ_;             // occupied-level bitmap (1 bit / level)

    long best_bid_idx_ = -1;                  // highest occupied bid level, or -1
    long best_ask_idx_ = 0;                   // lowest occupied ask level, or levels_.size()

    std::unordered_map<OrderId, Order*> order_index_;
    ArenaAllocator<Order>               order_arena_;

    SeqNum   next_sequence_     = 1;
    uint64_t trade_count_       = 0;
    uint64_t total_volume_      = 0;
    Price    last_trade_price_  = 0;

    std::vector<TradeCallback> trade_listeners_;
    std::vector<OrderCallback> order_listeners_;
};

} // namespace micro_exchange::core
