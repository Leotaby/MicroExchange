#pragma once

#include "Order.h"
#include "PriceLevel.h"
#include "ArenaAllocator.h"

#include <map>
#include <unordered_map>
#include <vector>
#include <functional>
#include <optional>
#include <string>
#include <cassert>

namespace micro_exchange::core {

/**
 * OrderBook — Central Limit Order Book (CLOB) with price-time priority.
 *
 * Design rationale:
 * ─────────────────
 * The book is organized as two sorted maps of PriceLevels (bids descending,
 * asks ascending). Within each level, orders are queued in FIFO order via
 * PriceLevel's intrusive linked list.
 *
 * Data structure choice — std::map vs alternatives:
 *
 *   • std::map<Price, PriceLevel>: O(log N) lookup by price. For typical
 *     equity books with 20-50 active levels, log₂(50) ≈ 6 comparisons.
 *     The real cost is cache misses from tree traversal.
 *
 *   • Alternative: contiguous array indexed by (price - min_price) / tick_size.
 *     O(1) lookup, perfect cache locality for BBO scan. Used in production
 *     exchanges (e.g., LMAX). We use std::map for clarity; the array
 *     optimization is documented as a design note.
 *
 *   • We additionally maintain a hash map from OrderId → Order* for O(1)
 *     cancel/amend operations.
 *
 * Matching algorithm:
 *   1. Incoming order scans the opposite side from best price inward
 *   2. At each level, match against FIFO queue from front
 *   3. Generate Trade for each fill
 *   4. Remove filled orders, update partial fills
 *   5. If incoming order has remaining quantity and is a limit order, rest it
 *
 * Invariants (verified by property-based tests):
 *   • No crossed book: best_bid < best_ask after every match cycle
 *   • FIFO: within a price level, earlier orders fill first
 *   • Determinism: given identical input sequence, output is identical
 *   • Conservation: total filled quantity on both sides of every trade is equal
 */
class OrderBook {
public:
    // Trade callback: invoked for each execution
    using TradeCallback = std::function<void(const Trade&)>;

    // Order update callback: invoked for status changes
    using OrderCallback = std::function<void(const Order&)>;

    explicit OrderBook(const std::string& symbol = "")
        : symbol_(symbol)
        , order_arena_(65536)
    {}

    // ───────────────────────────────────────────
    // Multi-subscriber dispatch
    //
    // The legacy set_*_callback API replaces a single slot, which made it
    // impossible to attach the matching engine and a feed publisher to the
    // same book without one clobbering the other. add_*_listener appends
    // to a fan-out list and is what new code should use.
    // ───────────────────────────────────────────
    void add_trade_listener(TradeCallback cb) {
        trade_listeners_.push_back(std::move(cb));
    }
    void add_order_listener(OrderCallback cb) {
        order_listeners_.push_back(std::move(cb));
    }
    void clear_listeners() {
        trade_listeners_.clear();
        order_listeners_.clear();
    }

    // ═══════════════════════════════════════════
    // Order Operations
    // ═══════════════════════════════════════════

    /**
     * Submit a new order. Attempts matching, then rests remainder if limit.
     * Returns the order pointer (owned by arena).
     */
    Order* add_order(const NewOrderRequest& req) {
        // Allocate from arena (zero malloc)
        Order* order = order_arena_.allocate();
        new (order) Order{};

        order->id        = req.id;
        order->sequence  = next_sequence_++;
        order->side      = req.side;
        order->type      = req.type;
        order->tif       = req.tif;
        order->price     = req.price;
        order->stop_price = req.stop_price;
        order->quantity  = req.quantity;
        order->leaves_qty = req.quantity;
        order->filled_qty = 0;
        order->entry_time = now();
        order->last_update = order->entry_time;
        order->status    = OrderStatus::New;
        std::memcpy(order->symbol, req.symbol, sizeof(order->symbol));

        // Index by ID for O(1) cancel/amend
        order_index_[order->id] = order;

        // Stop / StopLimit orders are parked until last-traded price crosses
        // the trigger. We park them first; if the trigger is already in the
        // money relative to the current best they will be released by the
        // explicit check_stop_triggers() call below.
        if (order->type == OrderType::Stop || order->type == OrderType::StopLimit) {
            park_stop_order(order);
            notify_order(*order);
            check_stop_triggers();
            return order;
        }

        // Attempt matching
        match(order);

        // Handle post-match: rest or cancel based on type
        if (order->leaves_qty > 0) {
            switch (order->type) {
                case OrderType::Limit:
                    rest_order(order);
                    break;
                case OrderType::Market:
                case OrderType::IOC:
                    // Cancel unfilled remainder
                    order->cancel();
                    order_index_.erase(order->id);
                    notify_order(*order);
                    break;
                case OrderType::FOK:
                    // Should have been fully filled or not at all
                    // (FOK pre-check happens in match())
                    order->cancel();
                    order_index_.erase(order->id);
                    notify_order(*order);
                    break;
                case OrderType::Stop:
                case OrderType::StopLimit:
                    // Handled above before reaching match() — should not get here.
                    break;
            }
        }

        // Each successful aggressive cycle may move the last-traded price and
        // therefore activate parked stop orders.
        check_stop_triggers();

        return order;
    }

    /**
     * Cancel an existing order. O(1) lookup + O(1) removal from level.
     */
    bool cancel_order(OrderId id) {
        auto it = order_index_.find(id);
        if (it == order_index_.end()) return false;

        Order* order = it->second;
        if (!order->is_active()) return false;

        // Stop orders live in stop_orders_, not in the price levels
        if (order->type == OrderType::Stop || order->type == OrderType::StopLimit) {
            unpark_stop_order(order);
            order->cancel();
            order_index_.erase(it);
            notify_order(*order);
            return true;
        }

        // Remove from price level
        remove_from_book(order);

        order->cancel();
        order_index_.erase(it);

        notify_order(*order);
        return true;
    }

    /**
     * Amend price and/or quantity. Price change = cancel + re-insert (loses priority).
     * Quantity reduction preserves priority.
     */
    bool amend_order(const AmendRequest& req) {
        auto it = order_index_.find(req.order_id);
        if (it == order_index_.end()) return false;

        Order* order = it->second;
        if (!order->is_active()) return false;

        bool price_changed = (req.new_price != 0 && req.new_price != order->price);
        bool qty_increased = (req.new_quantity != 0 && req.new_quantity > order->leaves_qty);

        if (price_changed || qty_increased) {
            // Loses queue priority: remove and re-insert
            remove_from_book(order);

            if (req.new_price != 0) order->price = req.new_price;
            if (req.new_quantity != 0) {
                order->quantity = req.new_quantity;
                order->leaves_qty = req.new_quantity - order->filled_qty;
            }
            order->sequence = next_sequence_++;
            order->status = OrderStatus::Amended;
            order->last_update = now();

            // Re-match then rest
            match(order);
            if (order->leaves_qty > 0 && order->type == OrderType::Limit) {
                rest_order(order);
            }
        } else if (req.new_quantity != 0 && req.new_quantity < order->leaves_qty) {
            // Quantity reduction: preserves priority
            Quantity reduction = order->leaves_qty - req.new_quantity;
            order->leaves_qty = req.new_quantity;
            order->quantity -= reduction;
            order->status = OrderStatus::Amended;
            order->last_update = now();

            // Update level aggregate
            auto& levels = order->is_buy() ? bids_ : asks_;
            auto level_it = levels.find(order->price);
            if (level_it != levels.end()) {
                level_it->second.reduce_quantity(reduction);
            }
        }

        notify_order(*order);
        return true;
    }

    // ═══════════════════════════════════════════
    // Book State Queries
    // ═══════════════════════════════════════════

    [[nodiscard]] std::optional<Price> best_bid() const {
        if (bids_.empty()) return std::nullopt;
        return bids_.rbegin()->first;  // Highest bid
    }

    [[nodiscard]] std::optional<Price> best_ask() const {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->first;   // Lowest ask
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

    [[nodiscard]] Quantity bid_depth(size_t levels = 0) const {
        return side_depth(bids_, levels);
    }

    [[nodiscard]] Quantity ask_depth(size_t levels = 0) const {
        return side_depth(asks_, levels);
    }

    struct BookLevel {
        Price    price;
        Quantity quantity;
        uint32_t order_count;
    };

    [[nodiscard]] std::vector<BookLevel> get_bids(size_t max_levels = 10) const {
        return get_side_levels(bids_, max_levels, true);
    }

    [[nodiscard]] std::vector<BookLevel> get_asks(size_t max_levels = 10) const {
        return get_side_levels(asks_, max_levels, false);
    }

    // ═══════════════════════════════════════════
    // Callbacks
    // ═══════════════════════════════════════════

    // Backwards-compatible single-slot setters. These now simply append to
    // the listener fan-out so older code that calls set_* still works AND
    // does not clobber any other subscribers. Prefer add_*_listener.
    void set_trade_callback(TradeCallback cb) { add_trade_listener(std::move(cb)); }
    void set_order_callback(OrderCallback cb) { add_order_listener(std::move(cb)); }

    // ═══════════════════════════════════════════
    // Statistics
    // ═══════════════════════════════════════════

    [[nodiscard]] uint64_t trade_count()    const { return trade_count_; }
    [[nodiscard]] uint64_t total_volume()   const { return total_volume_; }
    [[nodiscard]] SeqNum   sequence()       const { return next_sequence_; }
    [[nodiscard]] size_t   active_orders()  const { return order_index_.size(); }
    [[nodiscard]] const std::string& symbol() const { return symbol_; }
    [[nodiscard]] Price    last_trade_price()    const { return last_trade_price_; }
    [[nodiscard]] uint64_t stop_triggered_count() const { return stop_triggered_count_; }
    [[nodiscard]] size_t   parked_stop_count()   const {
        return buy_stops_.size() + sell_stops_.size();
    }

    // ═══════════════════════════════════════════
    // Invariant Checks (for testing)
    // ═══════════════════════════════════════════

    /**
     * Verify the book is not crossed: best_bid < best_ask.
     * Must hold after every match cycle.
     */
    [[nodiscard]] bool check_no_crossed_book() const {
        auto bb = best_bid();
        auto ba = best_ask();
        if (!bb || !ba) return true;  // One side empty = not crossed
        return *bb < *ba;
    }

    /**
     * Verify FIFO ordering within each price level.
     */
    [[nodiscard]] bool check_fifo_invariant() const {
        auto check_side = [](const auto& side) {
            for (const auto& [price, level] : side) {
                SeqNum prev_seq = 0;
                for (const auto& order : level) {
                    if (order.sequence <= prev_seq) return false;
                    prev_seq = order.sequence;
                }
            }
            return true;
        };
        return check_side(bids_) && check_side(asks_);
    }

private:
    // TODO: this should really be a contiguous array indexed by
    // (price - min_price) / tick_size for O(1) BBO lookup.
    // std::map is fine for now but the tree traversal kills cache locality.
    // See LMAX disruptor papers for how real exchanges do this.
    using BidMap = std::map<Price, PriceLevel, std::less<Price>>;
    using AskMap = std::map<Price, PriceLevel, std::less<Price>>;

    // ── Matching logic ──

    void match(Order* incoming) {
        if (incoming->type == OrderType::FOK) {
            // FOK: check if full fill is possible before executing
            if (!can_fill_completely(incoming)) {
                return;  // Will be cancelled by caller
            }
        }

        if (incoming->is_buy()) {
            match_against(incoming, asks_, [](Price order_price, Price level_price) {
                return order_price >= level_price || order_price == PRICE_MARKET;
            });
        } else {
            match_against(incoming, bids_, [](Price order_price, Price level_price) {
                return order_price <= level_price || order_price == PRICE_MARKET;
            });
        }
    }

    template <typename SideMap, typename PriceCheck>
    void match_against(Order* incoming, SideMap& contra_side, PriceCheck price_ok) {
        // Walk the best level repeatedly. We compute `it` inside the loop —
        // computing std::prev(end()) on an empty map is UB and ASan catches
        // it (the original code did this once at function entry).
        while (incoming->leaves_qty > 0 && !contra_side.empty()) {
            auto it = incoming->is_buy()
                          ? contra_side.begin()
                          : std::prev(contra_side.end());

            PriceLevel& level = it->second;

            if (!price_ok(incoming->price, level.price())) {
                break;  // No more matchable levels
            }

            // Match against orders in FIFO order
            while (incoming->leaves_qty > 0 && !level.empty()) {
                Order* resting = level.front();

                Quantity fill_qty = std::min(incoming->leaves_qty, resting->leaves_qty);

                // Execute trade
                Trade trade{};
                trade.sequence = next_sequence_++;
                trade.price = resting->price;  // Resting order's price (price improvement)
                trade.quantity = fill_qty;
                trade.exec_time = now();
                trade.aggressor = incoming->side;
                std::memcpy(trade.symbol, incoming->symbol, sizeof(trade.symbol));

                if (incoming->is_buy()) {
                    trade.buy_order_id = incoming->id;
                    trade.sell_order_id = resting->id;
                } else {
                    trade.buy_order_id = resting->id;
                    trade.sell_order_id = incoming->id;
                }

                // Update quantities
                // NOTE: we must update the level's total BEFORE fill() changes leaves_qty
                // because fill() modifies the order in-place and the level still references it.
                // Learned this the hard way (assertion failures in PriceLevel::reduce_quantity).
                level.reduce_quantity(fill_qty);
                incoming->fill(fill_qty);
                resting->fill(fill_qty);

                // Notify
                notify_trade(trade);
                notify_order(*resting);

                ++trade_count_;
                total_volume_ += fill_qty;
                last_trade_price_ = trade.price;

                // Remove fully filled resting order
                if (resting->is_filled()) {
                    level.pop_front();
                    order_index_.erase(resting->id);
                    // Note: we don't deallocate yet — arena manages lifetime
                }
            }

            // Remove empty level
            if (level.empty()) {
                contra_side.erase(it);
            }
        }
    }

    bool can_fill_completely(const Order* order) const {
        Quantity needed = order->leaves_qty;
        const auto& contra = order->is_buy() ? asks_ : bids_;

        for (const auto& [price, level] : contra) {
            if (order->is_buy() && order->price < price && order->price != PRICE_MARKET) break;
            if (!order->is_buy() && order->price > price && order->price != PRICE_MARKET) break;

            needed -= std::min(needed, level.total_quantity());
            if (needed == 0) return true;
        }
        return needed == 0;
    }

    // ── Book management ──

    void rest_order(Order* order) {
        auto& levels = order->is_buy() ? bids_ : asks_;
        auto [it, inserted] = levels.try_emplace(order->price, order->price);
        it->second.push_back(order);
    }

    // ── Stop-order parking ──
    //
    // Buy stops trigger when last_trade_price_ >= stop_price (price rising
    // through the stop). Sell stops trigger when last_trade_price_ <=
    // stop_price. We keep two ordered sets keyed by stop_price so the next
    // order to release is always at one end.
    void park_stop_order(Order* order) {
        order->status = OrderStatus::New;
        if (order->is_buy()) {
            buy_stops_.insert({order->stop_price, order});
        } else {
            sell_stops_.insert({order->stop_price, order});
        }
    }

    void unpark_stop_order(Order* order) {
        auto& set = order->is_buy() ? buy_stops_ : sell_stops_;
        auto range = set.equal_range(order->stop_price);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second == order) {
                set.erase(it);
                return;
            }
        }
    }

    // Activate any parked stop orders whose trigger has been crossed by the
    // most recent print. Triggered orders re-enter the book as Market (Stop)
    // or Limit (StopLimit) orders, which may themselves print and recurse —
    // we use a re-entry guard to keep that bounded.
    void check_stop_triggers() {
        if (in_stop_check_) return;
        in_stop_check_ = true;

        std::vector<Order*> to_release;

        while (true) {
            to_release.clear();

            // Buy stops fire when last print >= stop_price (lowest stops first)
            while (!buy_stops_.empty() &&
                   buy_stops_.begin()->first <= last_trade_price_) {
                to_release.push_back(buy_stops_.begin()->second);
                buy_stops_.erase(buy_stops_.begin());
            }
            // Sell stops fire when last print <= stop_price (highest stops first)
            while (!sell_stops_.empty() &&
                   sell_stops_.rbegin()->first >= last_trade_price_ &&
                   last_trade_price_ != 0) {
                auto it = std::prev(sell_stops_.end());
                to_release.push_back(it->second);
                sell_stops_.erase(it);
            }

            if (to_release.empty()) break;

            for (Order* o : to_release) {
                if (o->type == OrderType::Stop) {
                    o->type = OrderType::Market;
                    o->price = PRICE_MARKET;
                    o->tif   = TimeInForce::IOC;
                } else { // StopLimit
                    o->type = OrderType::Limit;
                    o->tif  = TimeInForce::GTC;
                }
                o->sequence    = next_sequence_++;
                o->last_update = now();
                ++stop_triggered_count_;

                match(o);

                if (o->leaves_qty > 0) {
                    if (o->type == OrderType::Limit) {
                        rest_order(o);
                    } else {
                        o->cancel();
                        order_index_.erase(o->id);
                        notify_order(*o);
                    }
                } else {
                    notify_order(*o);
                }
            }
        }

        in_stop_check_ = false;
    }

    void remove_from_book(Order* order) {
        auto& levels = order->is_buy() ? bids_ : asks_;
        auto it = levels.find(order->price);
        if (it != levels.end()) {
            it->second.remove(order);
            if (it->second.empty()) {
                levels.erase(it);
            }
        }
    }

    Quantity side_depth(const auto& side, size_t max_levels) const {
        Quantity total = 0;
        size_t count = 0;
        for (const auto& [price, level] : side) {
            total += level.total_quantity();
            if (max_levels > 0 && ++count >= max_levels) break;
        }
        return total;
    }

    std::vector<BookLevel> get_side_levels(const auto& side, size_t max_levels, bool reverse) const {
        std::vector<BookLevel> result;
        result.reserve(max_levels);

        if (reverse) {
            size_t count = 0;
            for (auto it = side.rbegin(); it != side.rend() && count < max_levels; ++it, ++count) {
                result.push_back({it->first, it->second.total_quantity(), it->second.order_count()});
            }
        } else {
            size_t count = 0;
            for (auto it = side.begin(); it != side.end() && count < max_levels; ++it, ++count) {
                result.push_back({it->first, it->second.total_quantity(), it->second.order_count()});
            }
        }
        return result;
    }

    // ── Members ──
    std::string symbol_;
    BidMap      bids_;
    AskMap      asks_;

    std::unordered_map<OrderId, Order*> order_index_;
    ArenaAllocator<Order>               order_arena_;

    SeqNum   next_sequence_ = 1;
    uint64_t trade_count_   = 0;
    uint64_t total_volume_  = 0;
    Price    last_trade_price_     = 0;
    uint64_t stop_triggered_count_ = 0;
    bool     in_stop_check_        = false;

    // Buy stops keyed ascending: lowest stop_price triggers first.
    // Sell stops keyed ascending: highest stop_price triggers first
    // (we walk from rbegin in check_stop_triggers).
    std::multimap<Price, Order*> buy_stops_;
    std::multimap<Price, Order*> sell_stops_;

    std::vector<TradeCallback> trade_listeners_;
    std::vector<OrderCallback> order_listeners_;

    void notify_trade(const Trade& t) {
        for (auto& cb : trade_listeners_) cb(t);
    }
    void notify_order(const Order& o) {
        for (auto& cb : order_listeners_) cb(o);
    }
};

} // namespace micro_exchange::core
