#pragma once

#include "Order.h"
#include <cstdint>
#include <cstring>

namespace micro_exchange::md {

using namespace micro_exchange::core;

/**
 * Feed message types — modeled after NASDAQ ITCH 5.0 protocol.
 *
 * The wire protocol uses tagged union messages with a fixed header.
 * In production, these would be serialized to a binary format with
 * network byte order. Here we use the in-memory representation directly.
 *
 * Message types:
 *   A — Add order (new resting order)
 *   X — Order executed (trade)
 *   D — Order deleted (cancel or fill)
 *   U — Order replaced (amend)
 *   S — Snapshot (full book state)
 *   T — Trade (execution report)
 *   Q — Quote update (BBO change)
 */

enum class FeedMessageType : uint8_t {
    AddOrder        = 'A',
    ExecuteOrder    = 'X',
    DeleteOrder     = 'D',
    ReplaceOrder    = 'U',
    Snapshot        = 'S',
    Trade           = 'T',
    QuoteUpdate     = 'Q',
    SystemEvent     = 'E',
};

/**
 * Fixed-size feed message for zero-copy transport over the SPSC buffer.
 * 128 bytes — fits two cache lines.
 */
struct alignas(64) FeedMessage {
    // ── Header (common to all message types) ──
    FeedMessageType type     = FeedMessageType::SystemEvent;
    SeqNum          sequence = 0;
    uint64_t        timestamp_ns = 0;  // Nanoseconds since epoch
    char            symbol[16] = {};

    // ── Payload (union-style, type-dependent) ──
    // We use a flat struct with all fields; unused fields are zero.
    // This avoids the complexity of std::variant in the hot path.

    OrderId  order_id     = 0;
    Side     side         = Side::Buy;
    Price    price        = 0;
    Quantity quantity     = 0;
    Quantity leaves_qty   = 0;

    // For trades
    OrderId  match_id     = 0;   // Counter-party order
    Side     aggressor_side = Side::Buy;

    // For snapshots
    Price    best_bid     = 0;
    Price    best_ask     = 0;
    Quantity bid_depth    = 0;
    Quantity ask_depth    = 0;

    // For quote updates (BBO)
    Price    bid_price    = 0;
    Price    ask_price    = 0;
    Quantity bid_size     = 0;
    Quantity ask_size     = 0;

    // ── Factory methods ──

    static FeedMessage make_add(SeqNum seq, const Order& order) {
        FeedMessage msg{};
        msg.type = FeedMessageType::AddOrder;
        msg.sequence = seq;
        msg.timestamp_ns = timestamp_ns_val(order.entry_time);
        std::memcpy(msg.symbol, order.symbol, sizeof(msg.symbol));
        msg.order_id = order.id;
        msg.side = order.side;
        msg.price = order.price;
        msg.quantity = order.leaves_qty;
        return msg;
    }

    static FeedMessage make_trade(SeqNum seq, const Trade& trade) {
        FeedMessage msg{};
        msg.type = FeedMessageType::Trade;
        msg.sequence = seq;
        msg.timestamp_ns = timestamp_ns_val(trade.exec_time);
        std::memcpy(msg.symbol, trade.symbol, sizeof(msg.symbol));
        msg.order_id = trade.buy_order_id;
        msg.match_id = trade.sell_order_id;
        msg.price = trade.price;
        msg.quantity = trade.quantity;
        msg.aggressor_side = trade.aggressor;
        return msg;
    }

    static FeedMessage make_delete(SeqNum seq, const Order& order) {
        FeedMessage msg{};
        msg.type = FeedMessageType::DeleteOrder;
        msg.sequence = seq;
        msg.timestamp_ns = timestamp_ns_val(order.last_update);
        std::memcpy(msg.symbol, order.symbol, sizeof(msg.symbol));
        msg.order_id = order.id;
        msg.side = order.side;
        msg.price = order.price;
        return msg;
    }

    static FeedMessage make_quote(SeqNum seq, const char* sym,
                                   Price bid_p, Quantity bid_s,
                                   Price ask_p, Quantity ask_s) {
        FeedMessage msg{};
        msg.type = FeedMessageType::QuoteUpdate;
        msg.sequence = seq;
        msg.timestamp_ns = timestamp_ns_val(now());
        std::memcpy(msg.symbol, sym, std::min(strlen(sym), sizeof(msg.symbol)));
        msg.bid_price = bid_p;
        msg.bid_size = bid_s;
        msg.ask_price = ask_p;
        msg.ask_size = ask_s;
        return msg;
    }

private:
    static uint64_t timestamp_ns_val(Timestamp ts) {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                ts.time_since_epoch()
            ).count()
        );
    }
};

static_assert(sizeof(FeedMessage) <= 256,
    "FeedMessage should fit in a small number of cache lines");

} // namespace micro_exchange::md
