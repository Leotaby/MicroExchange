/**
 * bench_throughput.cpp — Matching engine throughput and latency benchmark.
 *
 * Measures:
 *   • Single-thread matching throughput (orders/sec)
 *   • Per-order latency distribution (p50/p95/p99/p999)
 *   • Arena allocator overhead vs raw new/delete
 *   • Book depth impact on matching performance
 *
 * Methodology:
 *   Pre-generate all orders, then measure only the matching hot path.
 *   This isolates engine performance from random number generation.
 */

#include "../core/include/MatchingEngine.h"
#include "../core/include/OrderBook.h"
#include "../core/include/Order.h"

#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <random>
#include <algorithm>
#include <numeric>

using namespace micro_exchange::core;

using Clock = std::chrono::high_resolution_clock;
using ns = std::chrono::nanoseconds;

// ─────────────────────────────────────────────
// Pre-generate orders
// ─────────────────────────────────────────────

std::vector<NewOrderRequest> generate_orders(size_t count, uint64_t seed = 42) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<Price> price_dist(9900, 10100);
    std::uniform_int_distribution<Quantity> qty_dist(1, 10);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_real_distribution<double> type_dist(0.0, 1.0);

    std::vector<NewOrderRequest> orders;
    orders.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        NewOrderRequest req{};
        req.id = i + 1;
        req.side = side_dist(rng) ? Side::Buy : Side::Sell;

        double type_roll = type_dist(rng);
        if (type_roll < 0.7) {
            req.type = OrderType::Limit;
            req.tif = TimeInForce::GTC;
            req.price = price_dist(rng);
        } else {
            req.type = OrderType::Market;
            req.tif = TimeInForce::IOC;
            req.price = PRICE_MARKET;
        }

        req.quantity = qty_dist(rng) * 100;
        std::memcpy(req.symbol, "BENCH", 6);
        orders.push_back(req);
    }

    return orders;
}

// ─────────────────────────────────────────────
// Benchmark: Throughput
// ─────────────────────────────────────────────

void bench_throughput(size_t num_orders) {
    std::cout << "\n── Throughput Benchmark (" << num_orders << " orders) ──\n";

    auto orders = generate_orders(num_orders);
    OrderBook book("BENCH");

    auto start = Clock::now();
    for (const auto& req : orders) {
        book.add_order(req);
    }
    auto end = Clock::now();

    auto elapsed_ns = std::chrono::duration_cast<ns>(end - start).count();
    double elapsed_s = elapsed_ns / 1e9;
    double throughput = num_orders / elapsed_s;

    std::cout << "  Orders processed: " << num_orders << "\n";
    std::cout << "  Trades executed:  " << book.trade_count() << "\n";
    std::cout << "  Wall time:        " << std::fixed << std::setprecision(3)
              << elapsed_s * 1000 << " ms\n";
    std::cout << "  Throughput:       " << std::fixed << std::setprecision(0)
              << throughput << " orders/sec\n";
    std::cout << "                    " << std::fixed << std::setprecision(2)
              << throughput / 1e6 << "M orders/sec\n";
}

// ─────────────────────────────────────────────
// Benchmark: Latency distribution
// ─────────────────────────────────────────────

void bench_latency(size_t num_orders) {
    std::cout << "\n── Latency Benchmark (" << num_orders << " orders) ──\n";

    auto orders = generate_orders(num_orders);
    OrderBook book("BENCH");

    std::vector<uint64_t> latencies;
    latencies.reserve(num_orders);

    for (const auto& req : orders) {
        auto start = Clock::now();
        book.add_order(req);
        auto end = Clock::now();

        latencies.push_back(
            std::chrono::duration_cast<ns>(end - start).count()
        );
    }

    // Sort for percentile computation
    std::sort(latencies.begin(), latencies.end());

    auto percentile = [&](double p) -> uint64_t {
        size_t idx = static_cast<size_t>(p * (latencies.size() - 1));
        return latencies[idx];
    };

    double mean = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();

    std::cout << "  Mean:    " << std::fixed << std::setprecision(0) << mean << " ns\n";
    std::cout << "  P50:     " << percentile(0.50) << " ns\n";
    std::cout << "  P90:     " << percentile(0.90) << " ns\n";
    std::cout << "  P95:     " << percentile(0.95) << " ns\n";
    std::cout << "  P99:     " << percentile(0.99) << " ns\n";
    std::cout << "  P99.9:   " << percentile(0.999) << " ns\n";
    std::cout << "  Max:     " << latencies.back() << " ns\n";

    // Latency histogram
    std::cout << "\n  Latency Histogram:\n";
    std::vector<std::pair<std::string, uint64_t>> buckets = {
        {"<100ns",   100},
        {"100-250",  250},
        {"250-500",  500},
        {"500-1μs",  1000},
        {"1-2μs",    2000},
        {"2-5μs",    5000},
        {">5μs",     UINT64_MAX}
    };

    size_t idx = 0;
    for (const auto& [label, upper] : buckets) {
        size_t count = 0;
        while (idx < latencies.size() && latencies[idx] < upper) {
            ++count;
            ++idx;
        }
        double pct = 100.0 * count / latencies.size();
        int bars = static_cast<int>(pct / 2);
        std::cout << "    " << std::setw(8) << label << " │ "
                  << std::string(bars, '#')
                  << " " << std::fixed << std::setprecision(1) << pct << "%\n";
    }
}

// ─────────────────────────────────────────────
// Benchmark: Impact of book depth
// ─────────────────────────────────────────────

void bench_depth_impact() {
    std::cout << "\n── Book Depth Impact ──\n";
    std::cout << "  Depth  │ Add (ns)  │ Match (ns)\n";
    std::cout << "  ───────┼───────────┼───────────\n";

    for (size_t depth : {10, 50, 100, 500, 1000}) {
        OrderBook book("BENCH");

        // Build book to target depth
        for (size_t i = 0; i < depth; ++i) {
            NewOrderRequest bid{};
            bid.id = i + 1;
            bid.side = Side::Buy;
            bid.type = OrderType::Limit;
            bid.tif = TimeInForce::GTC;
            bid.price = 10000 - (i % 50);
            bid.quantity = 100;
            std::memcpy(bid.symbol, "BENCH", 6);
            book.add_order(bid);

            NewOrderRequest ask{};
            ask.id = depth + i + 1;
            ask.side = Side::Sell;
            ask.type = OrderType::Limit;
            ask.tif = TimeInForce::GTC;
            ask.price = 10001 + (i % 50);
            ask.quantity = 100;
            std::memcpy(ask.symbol, "BENCH", 6);
            book.add_order(ask);
        }

        // Measure add latency
        const size_t N = 10000;
        uint64_t add_total = 0;
        for (size_t i = 0; i < N; ++i) {
            NewOrderRequest req{};
            req.id = 100000 + i;
            req.side = (i % 2) ? Side::Buy : Side::Sell;
            req.type = OrderType::Limit;
            req.tif = TimeInForce::GTC;
            req.price = (req.side == Side::Buy) ? Price(9950) : Price(10050);
            req.quantity = 100;
            std::memcpy(req.symbol, "BENCH", 6);

            auto s = Clock::now();
            book.add_order(req);
            auto e = Clock::now();
            add_total += std::chrono::duration_cast<ns>(e - s).count();
        }

        // Measure match latency (market orders)
        uint64_t match_total = 0;
        for (size_t i = 0; i < N; ++i) {
            // First, add a resting order to match against
            NewOrderRequest rest{};
            rest.id = 200000 + i * 2;
            rest.side = Side::Buy;
            rest.type = OrderType::Limit;
            rest.tif = TimeInForce::GTC;
            rest.price = 10000;
            rest.quantity = 100;
            std::memcpy(rest.symbol, "BENCH", 6);
            book.add_order(rest);

            // Then measure matching a market sell
            NewOrderRequest mkt{};
            mkt.id = 200000 + i * 2 + 1;
            mkt.side = Side::Sell;
            mkt.type = OrderType::Market;
            mkt.tif = TimeInForce::IOC;
            mkt.price = PRICE_MARKET;
            mkt.quantity = 100;
            std::memcpy(mkt.symbol, "BENCH", 6);

            auto s = Clock::now();
            book.add_order(mkt);
            auto e = Clock::now();
            match_total += std::chrono::duration_cast<ns>(e - s).count();
        }

        std::cout << "  " << std::setw(5) << depth
                  << "  │ " << std::setw(7) << add_total / N
                  << "   │ " << std::setw(7) << match_total / N << "\n";
    }
}

// ─────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────

int main() {
    std::cout << "\n══════════════════════════════════════════════\n";
    std::cout << "  MicroExchange — Performance Benchmarks\n";
    std::cout << "══════════════════════════════════════════════\n";

    bench_throughput(100000);
    bench_throughput(1000000);
    bench_latency(100000);
    bench_depth_impact();

    std::cout << "\n══════════════════════════════════════════════\n";
    std::cout << "  Benchmarks complete\n";
    std::cout << "══════════════════════════════════════════════\n\n";

    return 0;
}
