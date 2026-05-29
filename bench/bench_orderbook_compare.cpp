/**
 * bench_orderbook_compare.cpp — std::map OrderBook vs tick-indexed ArrayOrderBook.
 *
 * Two questions, answered side by side:
 *
 *   1. CORRECTNESS: do the two books produce the *identical* trade stream on
 *      the same input? (If not, any speed comparison is meaningless.) We run
 *      both, capture every (buy_id, sell_id, price, qty, aggressor) tuple, and
 *      assert the sequences match exactly.
 *
 *   2. PERFORMANCE: how much faster is the contiguous, cache-friendly array
 *      layout than the red-black tree on the matching hot path?
 *
 * Both books reuse the SAME Order, PriceLevel, and ArenaAllocator, so the only
 * variable is the level container (std::map vs flat array).
 *
 * Methodology: orders are pre-generated, so we time only the matching loop, not
 * RNG. Same seed and same workload feed both books.
 */

#include "../core/include/OrderBook.h"
#include "../core/include/ArrayOrderBook.h"
#include "../core/include/Order.h"

#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <random>
#include <algorithm>
#include <numeric>
#include <cstring>

using namespace micro_exchange::core;

using Clock = std::chrono::high_resolution_clock;
using ns    = std::chrono::nanoseconds;

// Price band for the workload. Limits land in [9900, 10100]; the array book is
// sized a little wider so every resting limit has a slot.
static constexpr Price kMinPrice = 9800;
static constexpr Price kMaxPrice = 10200;

// ─────────────────────────────────────────────
// Workload (same generator the throughput bench uses)
// ─────────────────────────────────────────────
std::vector<NewOrderRequest> generate_orders(size_t count, uint64_t seed = 42) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<Price>    price_dist(9900, 10100);
    std::uniform_int_distribution<Quantity> qty_dist(1, 10);
    std::uniform_int_distribution<int>      side_dist(0, 1);
    std::uniform_real_distribution<double>  type_dist(0.0, 1.0);

    std::vector<NewOrderRequest> orders;
    orders.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        NewOrderRequest req{};
        req.id   = i + 1;
        req.side = side_dist(rng) ? Side::Buy : Side::Sell;
        if (type_dist(rng) < 0.7) {
            req.type  = OrderType::Limit;
            req.tif   = TimeInForce::GTC;
            req.price = price_dist(rng);
        } else {
            req.type  = OrderType::Market;
            req.tif   = TimeInForce::IOC;
            req.price = PRICE_MARKET;
        }
        req.quantity = qty_dist(rng) * 100;
        std::memcpy(req.symbol, "BENCH", 6);
        orders.push_back(req);
    }
    return orders;
}

// Comparable trade fingerprint (sequence numbers excluded — they're per-book).
struct TradeKey {
    OrderId  buy_id, sell_id;
    Price    price;
    Quantity qty;
    uint8_t  aggressor;
    bool operator==(const TradeKey& o) const {
        return buy_id == o.buy_id && sell_id == o.sell_id &&
               price == o.price && qty == o.qty && aggressor == o.aggressor;
    }
};

template <class Book>
std::vector<TradeKey> collect_trades(Book& book, const std::vector<NewOrderRequest>& orders) {
    std::vector<TradeKey> trades;
    trades.reserve(orders.size());
    book.set_trade_callback([&](const Trade& t) {
        trades.push_back({t.buy_order_id, t.sell_order_id, t.price, t.quantity,
                          static_cast<uint8_t>(t.aggressor)});
    });
    for (const auto& r : orders) book.add_order(r);
    return trades;
}

template <class Book>
double time_throughput(Book& book, const std::vector<NewOrderRequest>& orders) {
    auto start = Clock::now();
    for (const auto& r : orders) book.add_order(r);
    auto end = Clock::now();
    double s = std::chrono::duration_cast<ns>(end - start).count() / 1e9;
    return orders.size() / s;
}

template <class Book>
void time_latency(Book& book, const std::vector<NewOrderRequest>& orders,
                  uint64_t& p50, uint64_t& p99) {
    std::vector<uint64_t> lat;
    lat.reserve(orders.size());
    for (const auto& r : orders) {
        auto s = Clock::now();
        book.add_order(r);
        auto e = Clock::now();
        lat.push_back(std::chrono::duration_cast<ns>(e - s).count());
    }
    std::sort(lat.begin(), lat.end());
    p50 = lat[static_cast<size_t>(0.50 * (lat.size() - 1))];
    p99 = lat[static_cast<size_t>(0.99 * (lat.size() - 1))];
}

int main() {
    std::cout << "\n══════════════════════════════════════════════════════════\n";
    std::cout << "  OrderBook (std::map)  vs  ArrayOrderBook (tick-indexed)\n";
    std::cout << "══════════════════════════════════════════════════════════\n";

    // ── 1. Correctness cross-check ──
    {
        auto orders = generate_orders(200000);
        OrderBook      map_book("BENCH");
        ArrayOrderBook arr_book("BENCH", kMinPrice, kMaxPrice);

        auto map_trades = collect_trades(map_book, orders);
        auto arr_trades = collect_trades(arr_book, orders);

        bool ok = (map_trades.size() == arr_trades.size()) &&
                  std::equal(map_trades.begin(), map_trades.end(), arr_trades.begin());

        std::cout << "\n── Correctness ──\n";
        std::cout << "  std::map  trades: " << map_trades.size() << "\n";
        std::cout << "  array     trades: " << arr_trades.size() << "\n";
        std::cout << "  identical trade stream: " << (ok ? "YES ✓" : "NO ✗") << "\n";
        if (!ok) {
            std::cout << "  FATAL: trade streams diverge — aborting benchmark.\n";
            return 1;
        }
    }

    // ── 2. Throughput ──
    std::cout << "\n── Throughput (orders/sec) ──\n";
    std::cout << "   N        │ std::map      │ array         │ speedup\n";
    std::cout << "  ──────────┼───────────────┼───────────────┼─────────\n";
    for (size_t n : {100000, 1000000}) {
        auto orders = generate_orders(n);
        OrderBook      map_book("BENCH");
        ArrayOrderBook arr_book("BENCH", kMinPrice, kMaxPrice);

        double map_tp = time_throughput(map_book, orders);
        double arr_tp = time_throughput(arr_book, orders);

        std::cout << "  " << std::setw(8) << n << "  │ "
                  << std::setw(10) << std::fixed << std::setprecision(0) << map_tp << "/s │ "
                  << std::setw(10) << arr_tp << "/s │ "
                  << std::setw(5) << std::setprecision(2) << (arr_tp / map_tp) << "x\n";
    }

    // ── 3. Per-order latency ──
    std::cout << "\n── Latency (ns/order, 1M orders) ──\n";
    {
        auto orders = generate_orders(1000000);
        OrderBook      map_book("BENCH");
        ArrayOrderBook arr_book("BENCH", kMinPrice, kMaxPrice);

        uint64_t m50, m99, a50, a99;
        time_latency(map_book, orders, m50, m99);
        time_latency(arr_book, orders, a50, a99);

        std::cout << "            │  P50   │  P99\n";
        std::cout << "  ──────────┼────────┼────────\n";
        std::cout << "  std::map  │ " << std::setw(5) << m50 << "  │ " << std::setw(5) << m99 << "\n";
        std::cout << "  array     │ " << std::setw(5) << a50 << "  │ " << std::setw(5) << a99 << "\n";
    }

    std::cout << "\n══════════════════════════════════════════════════════════\n";
    std::cout << "  Done.\n";
    std::cout << "══════════════════════════════════════════════════════════\n\n";
    return 0;
}
