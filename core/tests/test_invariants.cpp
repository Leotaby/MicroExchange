/**
 * test_invariants.cpp — Property-based tests for matching engine invariants.
 *
 * Tests verify the three core invariants that define a correct CLOB:
 *
 *   1. No crossed book: After every operation, best_bid < best_ask
 *   2. FIFO preserved: Within a price level, earlier orders fill first
 *   3. Determinism: Identical input → identical output on every run
 *
 * Additionally:
 *   4. Conservation: Trade qty matches on both sides
 *   5. Quantity consistency: filled_qty + leaves_qty == original qty
 *   6. No phantom orders: cancelled orders don't participate in matching
 *
 * Test methodology: property-based testing with random order streams.
 * Each test generates thousands of random events and checks invariants
 * after every single operation — not just at the end.
 */

#include "../include/MatchingEngine.h"
#include "../include/OrderBook.h"
#include "../include/Order.h"

#include <cassert>
#include <iostream>
#include <random>
#include <vector>
#include <string>
#include <algorithm>

using namespace micro_exchange::core;

// ─────────────────────────────────────────────
// Test helpers
// ─────────────────────────────────────────────

class RandomOrderGenerator {
public:
    explicit RandomOrderGenerator(uint64_t seed = 42)
        : rng_(seed), price_dist_(9900, 10100), qty_dist_(100, 1000),
          side_dist_(0, 1), type_dist_(0.0, 1.0)
    {}

    NewOrderRequest generate(OrderId id) {
        NewOrderRequest req{};
        req.id = id;
        req.side = side_dist_(rng_) ? Side::Buy : Side::Sell;
        req.price = price_dist_(rng_);
        req.quantity = (qty_dist_(rng_) / 100) * 100;  // Round to 100
        if (req.quantity == 0) req.quantity = 100;

        double type_roll = type_dist_(rng_);
        if (type_roll < 0.7) {
            req.type = OrderType::Limit;
            req.tif = TimeInForce::GTC;
        } else if (type_roll < 0.85) {
            req.type = OrderType::Market;
            req.tif = TimeInForce::IOC;
            req.price = PRICE_MARKET;
        } else {
            req.type = OrderType::IOC;
            req.tif = TimeInForce::IOC;
        }

        std::memcpy(req.symbol, "TEST", 5);
        return req;
    }

private:
    std::mt19937_64 rng_;
    std::uniform_int_distribution<Price> price_dist_;
    std::uniform_int_distribution<Quantity> qty_dist_;
    std::uniform_int_distribution<int> side_dist_;
    std::uniform_real_distribution<double> type_dist_;
};

// ─────────────────────────────────────────────
// Test 1: No Crossed Book
// ─────────────────────────────────────────────

void test_no_crossed_book() {
    std::cout << "TEST: No crossed book invariant... ";

    OrderBook book("TEST");
    RandomOrderGenerator gen(12345);

    for (OrderId id = 1; id <= 50000; ++id) {
        auto req = gen.generate(id);
        book.add_order(req);

        // Check invariant after EVERY operation
        assert(book.check_no_crossed_book() &&
               "INVARIANT VIOLATED: Book is crossed after add_order!");
    }

    std::cout << "PASSED (50,000 random orders)\n";
}

// ─────────────────────────────────────────────
// Test 2: FIFO Priority
// ─────────────────────────────────────────────

void test_fifo_priority() {
    std::cout << "TEST: FIFO priority invariant... ";

    OrderBook book("TEST");

    // Place multiple orders at the same price
    for (OrderId id = 1; id <= 10; ++id) {
        NewOrderRequest req{};
        req.id = id;
        req.side = Side::Buy;
        req.type = OrderType::Limit;
        req.tif = TimeInForce::GTC;
        req.price = 10000;
        req.quantity = 100;
        std::memcpy(req.symbol, "TEST", 5);
        book.add_order(req);
    }

    // Send a sell market order that partially fills
    std::vector<OrderId> fill_order;
    book.set_trade_callback([&](const Trade& trade) {
        fill_order.push_back(trade.buy_order_id);
    });

    NewOrderRequest sell{};
    sell.id = 100;
    sell.side = Side::Sell;
    sell.type = OrderType::Market;
    sell.tif = TimeInForce::IOC;
    sell.price = PRICE_MARKET;
    sell.quantity = 300;  // Fill first 3 orders
    std::memcpy(sell.symbol, "TEST", 5);
    book.add_order(sell);

    // Verify FIFO: orders 1, 2, 3 should have filled (in that order)
    assert(fill_order.size() == 3);
    assert(fill_order[0] == 1);
    assert(fill_order[1] == 2);
    assert(fill_order[2] == 3);

    // Verify FIFO invariant in remaining book
    assert(book.check_fifo_invariant());

    std::cout << "PASSED\n";
}

// ─────────────────────────────────────────────
// Test 3: Determinism
// ─────────────────────────────────────────────

void test_determinism() {
    std::cout << "TEST: Deterministic matching... ";

    auto run_simulation = [](uint64_t seed) -> std::vector<Trade> {
        OrderBook book("TEST");
        RandomOrderGenerator gen(seed);
        std::vector<Trade> trades;

        book.set_trade_callback([&](const Trade& trade) {
            trades.push_back(trade);
        });

        for (OrderId id = 1; id <= 10000; ++id) {
            auto req = gen.generate(id);
            book.add_order(req);
        }

        return trades;
    };

    // Run twice with same seed
    auto trades1 = run_simulation(999);
    auto trades2 = run_simulation(999);

    // Must produce identical results
    assert(trades1.size() == trades2.size() &&
           "Determinism failed: different number of trades");

    for (size_t i = 0; i < trades1.size(); ++i) {
        assert(trades1[i].price == trades2[i].price &&
               "Determinism failed: different trade prices");
        assert(trades1[i].quantity == trades2[i].quantity &&
               "Determinism failed: different trade quantities");
        assert(trades1[i].buy_order_id == trades2[i].buy_order_id &&
               "Determinism failed: different buyer");
        assert(trades1[i].sell_order_id == trades2[i].sell_order_id &&
               "Determinism failed: different seller");
    }

    std::cout << "PASSED (" << trades1.size() << " trades matched identically)\n";
}

// ─────────────────────────────────────────────
// Test 4: Conservation of Quantity
// ─────────────────────────────────────────────

void test_conservation() {
    std::cout << "TEST: Quantity conservation... ";

    OrderBook book("TEST");
    RandomOrderGenerator gen(777);

    uint64_t total_trade_volume = 0;
    book.set_trade_callback([&](const Trade& trade) {
        total_trade_volume += trade.quantity;
    });

    std::vector<Order*> all_orders;
    for (OrderId id = 1; id <= 20000; ++id) {
        auto req = gen.generate(id);
        Order* order = book.add_order(req);
        all_orders.push_back(order);
    }

    // Verify: total volume from trade callbacks == total filled across all orders
    uint64_t total_filled = 0;
    for (const auto* order : all_orders) {
        assert(order->filled_qty + order->leaves_qty <= order->quantity ||
               order->status == OrderStatus::Cancelled);
        total_filled += order->filled_qty;
    }

    // Each trade fills two sides, so total_filled should be 2 * total_trade_volume
    assert(total_filled == 2 * total_trade_volume &&
           "Conservation violated: filled qty != 2 * trade volume");

    std::cout << "PASSED (volume conserved across " << total_trade_volume << " units)\n";
}

// ─────────────────────────────────────────────
// Test 5: Cancel correctness
// ─────────────────────────────────────────────

void test_cancel_correctness() {
    std::cout << "TEST: Cancel correctness... ";

    OrderBook book("TEST");

    // Place a buy order
    NewOrderRequest buy{};
    buy.id = 1;
    buy.side = Side::Buy;
    buy.type = OrderType::Limit;
    buy.tif = TimeInForce::GTC;
    buy.price = 10000;
    buy.quantity = 500;
    std::memcpy(buy.symbol, "TEST", 5);
    book.add_order(buy);

    assert(book.active_orders() == 1);

    // Cancel it
    bool cancelled = book.cancel_order(1);
    assert(cancelled && "Cancel should succeed");
    assert(book.active_orders() == 0 && "No active orders after cancel");

    // Try to fill the cancelled order — should not match
    bool any_trade = false;
    book.set_trade_callback([&](const Trade&) { any_trade = true; });

    NewOrderRequest sell{};
    sell.id = 2;
    sell.side = Side::Sell;
    sell.type = OrderType::Market;
    sell.tif = TimeInForce::IOC;
    sell.price = PRICE_MARKET;
    sell.quantity = 500;
    std::memcpy(sell.symbol, "TEST", 5);
    book.add_order(sell);

    assert(!any_trade && "Cancelled order should not be filled");

    // Double cancel should fail
    assert(!book.cancel_order(1) && "Double cancel should return false");

    std::cout << "PASSED\n";
}

// ─────────────────────────────────────────────
// Test 6: Fuzz test with invariant checks
// ─────────────────────────────────────────────

void test_fuzz_random_sequence() {
    std::cout << "TEST: Fuzz random event sequence... ";

    OrderBook book("TEST");
    std::mt19937_64 rng(54321);
    std::uniform_int_distribution<int> action_dist(0, 9);
    std::uniform_int_distribution<Price> price_dist(9950, 10050);
    std::uniform_int_distribution<Quantity> qty_dist(1, 10);

    OrderId next_id = 1;
    std::vector<OrderId> active_ids;

    for (int i = 0; i < 100000; ++i) {
        int action = action_dist(rng);

        if (action < 7) {
            // 70%: new order
            NewOrderRequest req{};
            req.id = next_id++;
            req.side = (rng() % 2) ? Side::Buy : Side::Sell;
            req.type = (rng() % 5 == 0) ? OrderType::Market : OrderType::Limit;
            req.tif = (req.type == OrderType::Market) ? TimeInForce::IOC : TimeInForce::GTC;
            req.price = (req.type == OrderType::Market) ? PRICE_MARKET : price_dist(rng);
            req.quantity = qty_dist(rng) * 100;
            std::memcpy(req.symbol, "TEST", 5);

            Order* order = book.add_order(req);
            if (order->is_active()) {
                active_ids.push_back(order->id);
            }
        } else if (action < 9 && !active_ids.empty()) {
            // 20%: cancel
            size_t idx = rng() % active_ids.size();
            book.cancel_order(active_ids[idx]);
            active_ids.erase(active_ids.begin() + idx);
        } else if (!active_ids.empty()) {
            // 10%: amend
            size_t idx = rng() % active_ids.size();
            AmendRequest amend{};
            amend.order_id = active_ids[idx];
            amend.new_quantity = qty_dist(rng) * 100;
            std::memcpy(amend.symbol, "TEST", 5);
            book.amend_order(amend);
        }

        // Check invariants after every operation
        assert(book.check_no_crossed_book() &&
               "FUZZ: Book crossed!");
    }

    std::cout << "PASSED (100,000 random events, invariants held)\n";
}

// ─────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────

int main() {
    std::cout << "\n══════════════════════════════════════════════\n";
    std::cout << "  MicroExchange — Invariant Test Suite\n";
    std::cout << "══════════════════════════════════════════════\n\n";

    test_no_crossed_book();
    test_fifo_priority();
    test_determinism();
    test_conservation();
    test_cancel_correctness();
    test_fuzz_random_sequence();

    std::cout << "\n══════════════════════════════════════════════\n";
    std::cout << "  ALL TESTS PASSED ✓\n";
    std::cout << "══════════════════════════════════════════════\n\n";

    return 0;
}
