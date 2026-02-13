#pragma once

#include "FeedMessage.h"
#include "SPSCRingBuffer.h"
#include "OrderBook.h"

#include <vector>
#include <functional>
#include <fstream>
#include <string>

namespace micro_exchange::md {

using namespace micro_exchange::core;

/**
 * FeedPublisher — Publishes incremental book updates and snapshots.
 *
 * The publisher sits between the matching engine and downstream consumers
 * (analytics, logging, network dissemination). It transforms engine events
 * into a standardized feed protocol.
 *
 * Architecture:
 *   [MatchingEngine] → callbacks → [FeedPublisher] → SPSC buffer → [consumers]
 *
 * The publisher maintains sequence numbers for gap detection and supports
 * periodic snapshot generation for client recovery.
 */
class FeedPublisher {
public:
    static constexpr size_t BUFFER_SIZE = 1 << 16;  // 65536 messages

    using MessageCallback = std::function<void(const FeedMessage&)>;

    FeedPublisher() = default;

    /**
     * Wire up to an OrderBook's callbacks.
     */
    void attach(OrderBook& book) {
        book.set_trade_callback([this, &book](const Trade& trade) {
            publish_trade(trade);
            publish_bbo_update(book);
        });

        book.set_order_callback([this, &book](const Order& order) {
            if (order.status == OrderStatus::New || order.status == OrderStatus::Amended) {
                publish_add(order);
            } else if (order.status == OrderStatus::Cancelled) {
                publish_delete(order);
            }
            publish_bbo_update(book);
        });
    }

    /**
     * Generate a full book snapshot for recovery.
     */
    FeedMessage generate_snapshot(const OrderBook& book) {
        FeedMessage snap{};
        snap.type = FeedMessageType::Snapshot;
        snap.sequence = next_seq_++;

        auto bb = book.best_bid();
        auto ba = book.best_ask();
        snap.best_bid = bb.value_or(0);
        snap.best_ask = ba.value_or(0);
        snap.bid_depth = book.bid_depth();
        snap.ask_depth = book.ask_depth();

        const char* sym = book.symbol().c_str();
        std::memcpy(snap.symbol, sym, std::min(book.symbol().size(), size_t(16)));

        if (callback_) callback_(snap);
        messages_.push_back(snap);
        return snap;
    }

    void set_callback(MessageCallback cb) { callback_ = std::move(cb); }

    [[nodiscard]] const std::vector<FeedMessage>& messages() const { return messages_; }
    [[nodiscard]] SeqNum sequence() const { return next_seq_; }

    /**
     * Write all messages to binary file for replay.
     */
    void dump_to_file(const std::string& path) const {
        std::ofstream ofs(path, std::ios::binary);
        for (const auto& msg : messages_) {
            ofs.write(reinterpret_cast<const char*>(&msg), sizeof(FeedMessage));
        }
    }

    /**
     * Compute feed statistics.
     */
    struct FeedStats {
        uint64_t total_messages = 0;
        uint64_t add_count      = 0;
        uint64_t trade_count    = 0;
        uint64_t delete_count   = 0;
        uint64_t snapshot_count = 0;
        uint64_t quote_count    = 0;
    };

    [[nodiscard]] FeedStats get_stats() const {
        FeedStats s{};
        s.total_messages = messages_.size();
        for (const auto& m : messages_) {
            switch (m.type) {
                case FeedMessageType::AddOrder:    ++s.add_count; break;
                case FeedMessageType::Trade:       ++s.trade_count; break;
                case FeedMessageType::DeleteOrder:  ++s.delete_count; break;
                case FeedMessageType::Snapshot:     ++s.snapshot_count; break;
                case FeedMessageType::QuoteUpdate:  ++s.quote_count; break;
                default: break;
            }
        }
        return s;
    }

private:
    void publish_trade(const Trade& trade) {
        auto msg = FeedMessage::make_trade(next_seq_++, trade);
        if (callback_) callback_(msg);
        messages_.push_back(msg);
    }

    void publish_add(const Order& order) {
        auto msg = FeedMessage::make_add(next_seq_++, order);
        if (callback_) callback_(msg);
        messages_.push_back(msg);
    }

    void publish_delete(const Order& order) {
        auto msg = FeedMessage::make_delete(next_seq_++, order);
        if (callback_) callback_(msg);
        messages_.push_back(msg);
    }

    void publish_bbo_update(const OrderBook& book) {
        auto bb = book.best_bid();
        auto ba = book.best_ask();
        if (bb && ba) {
            auto bids = book.get_bids(1);
            auto asks = book.get_asks(1);
            auto msg = FeedMessage::make_quote(
                next_seq_++,
                book.symbol().c_str(),
                *bb, bids.empty() ? 0 : bids[0].quantity,
                *ba, asks.empty() ? 0 : asks[0].quantity
            );
            if (callback_) callback_(msg);
            messages_.push_back(msg);
        }
    }

    SeqNum next_seq_ = 1;
    MessageCallback callback_;
    std::vector<FeedMessage> messages_;
};

/**
 * FeedReplayer — Reads binary feed files and replays messages.
 */
class FeedReplayer {
public:
    using MessageCallback = std::function<void(const FeedMessage&)>;

    explicit FeedReplayer(const std::string& path)
        : path_(path)
    {}

    /**
     * Replay all messages, invoking callback for each.
     * Returns total message count.
     */
    size_t replay(MessageCallback cb) {
        std::ifstream ifs(path_, std::ios::binary);
        if (!ifs) return 0;

        size_t count = 0;
        FeedMessage msg;
        while (ifs.read(reinterpret_cast<char*>(&msg), sizeof(FeedMessage))) {
            if (cb) cb(msg);
            ++count;
        }
        return count;
    }

    /**
     * Load all messages into memory for analysis.
     */
    std::vector<FeedMessage> load_all() {
        std::vector<FeedMessage> messages;
        replay([&](const FeedMessage& msg) {
            messages.push_back(msg);
        });
        return messages;
    }

private:
    std::string path_;
};

} // namespace micro_exchange::md
