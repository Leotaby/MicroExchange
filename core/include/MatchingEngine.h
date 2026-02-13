#pragma once

#include "Order.h"
#include "OrderBook.h"

#include <unordered_map>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>

namespace micro_exchange::core {

/**
 * MatchingEngine — Multi-symbol matching engine facade.
 *
 * Thread safety model:
 * ────────────────────
 * The engine supports two threading models:
 *
 *   1. Single-threaded (default): All operations on one thread.
 *      This is the standard exchange model — events are processed
 *      sequentially from a single gateway queue. Determinism is trivial.
 *
 *   2. Per-symbol sharding: Each OrderBook can be assigned to a
 *      dedicated thread. Cross-symbol operations (rare in equity markets)
 *      require coordination. This is how CME and ICE scale.
 *
 * For the single-threaded hot path, we avoid all locking.
 * The optional mutex is for multi-threaded access patterns only.
 *
 * Sequencing:
 * ───────────
 * Every event (order, cancel, amend, trade) gets a monotonically increasing
 * sequence number. This enables:
 *   • Deterministic replay
 *   • Gap detection in market data feeds
 *   • Consistent ordering across undo/redo
 */
class MatchingEngine {
public:
    struct EngineStats {
        uint64_t total_orders    = 0;
        uint64_t total_cancels   = 0;
        uint64_t total_amends    = 0;
        uint64_t total_trades    = 0;
        uint64_t total_volume    = 0;
        uint64_t total_rejects   = 0;
        uint64_t active_orders   = 0;
        uint64_t symbols_active  = 0;
    };

    MatchingEngine() = default;

    // ═══════════════════════════════════════════
    // Symbol management
    // ═══════════════════════════════════════════

    /**
     * Register a tradeable symbol. Must be called before any orders.
     */
    OrderBook& add_symbol(const std::string& symbol) {
        auto [it, inserted] = books_.try_emplace(symbol, symbol);
        if (inserted) {
            // Wire up callbacks
            it->second.set_trade_callback([this](const Trade& trade) {
                on_trade(trade);
            });
        }
        return it->second;
    }

    [[nodiscard]] OrderBook* get_book(const std::string& symbol) {
        auto it = books_.find(symbol);
        return (it != books_.end()) ? &it->second : nullptr;
    }

    // ═══════════════════════════════════════════
    // Order entry
    // ═══════════════════════════════════════════

    Order* submit_order(const NewOrderRequest& req) {
        std::string sym(req.symbol, std::strlen(req.symbol));
        auto it = books_.find(sym);
        if (it == books_.end()) {
            ++stats_.total_rejects;
            return nullptr;
        }

        ++stats_.total_orders;
        return it->second.add_order(req);
    }

    bool cancel_order(const CancelRequest& req) {
        std::string sym(req.symbol, std::strlen(req.symbol));
        auto it = books_.find(sym);
        if (it == books_.end()) return false;

        bool success = it->second.cancel_order(req.order_id);
        if (success) ++stats_.total_cancels;
        return success;
    }

    bool amend_order(const AmendRequest& req) {
        std::string sym(req.symbol, std::strlen(req.symbol));
        auto it = books_.find(sym);
        if (it == books_.end()) return false;

        bool success = it->second.amend_order(req);
        if (success) ++stats_.total_amends;
        return success;
    }

    // ═══════════════════════════════════════════
    // Global trade callback
    // ═══════════════════════════════════════════

    using GlobalTradeCallback = std::function<void(const Trade&)>;

    void set_trade_callback(GlobalTradeCallback cb) {
        global_trade_callback_ = std::move(cb);
    }

    // ═══════════════════════════════════════════
    // Statistics
    // ═══════════════════════════════════════════

    [[nodiscard]] EngineStats get_stats() const {
        EngineStats s = stats_;
        s.active_orders = 0;
        s.symbols_active = books_.size();
        for (const auto& [sym, book] : books_) {
            s.active_orders += book.active_orders();
        }
        return s;
    }

    [[nodiscard]] const std::unordered_map<std::string, OrderBook>& books() const {
        return books_;
    }

private:
    void on_trade(const Trade& trade) {
        ++stats_.total_trades;
        stats_.total_volume += trade.quantity;
        if (global_trade_callback_) {
            global_trade_callback_(trade);
        }
    }

    std::unordered_map<std::string, OrderBook> books_;
    EngineStats stats_;
    GlobalTradeCallback global_trade_callback_;
};

} // namespace micro_exchange::core
