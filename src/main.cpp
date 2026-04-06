/*
 * main.cpp - MicroExchange CLI
 *
 * Runs the full pipeline: hawkes event generation -> ZI agents ->
 * matching engine -> feed publisher -> analytics.
 *
 * Usage:
 *   ./micro_exchange                      # default 1hr simulation
 *   ./micro_exchange --duration 7200      # 2hr simulation
 *   ./micro_exchange --orders 500000      # cap at 500k events
 *   ./micro_exchange --output results/    # custom output dir
 */

#include "MatchingEngine.h"
#include "OrderBook.h"
#include "Order.h"
#include "FeedPublisher.h"
#include "HawkesProcess.h"
#include "ZIAgent.h"
#include "SpreadAnalyzer.h"
#include "ImpactAnalyzer.h"
#include "StylizedFacts.h"

#include <iostream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <string>
#include <vector>
#include <numeric>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <filesystem>

using namespace micro_exchange::core;
using namespace micro_exchange::md;
using namespace micro_exchange::sim;
using namespace micro_exchange::analytics;

namespace fs = std::filesystem;

// ── Config ──

struct RunConfig {
    std::string symbol    = "AAPL";
    double      duration  = 3600.0;
    Price       init_mid  = 15000;  // $150.00
    size_t      n_agents  = 10;
    std::string out_dir   = "output";
    bool        verbose   = false;
};

RunConfig parse_args(int argc, char* argv[]) {
    RunConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--duration" && i + 1 < argc) cfg.duration = std::stod(argv[++i]);
        else if (arg == "--symbol" && i + 1 < argc) cfg.symbol = argv[++i];
        else if (arg == "--output" && i + 1 < argc) cfg.out_dir = argv[++i];
        else if (arg == "-v" || arg == "--verbose") cfg.verbose = true;
        else if (arg == "--help") {
            std::cout << "Usage: micro_exchange [--duration SEC] [--symbol SYM] [--output DIR] [-v]\n";
            std::exit(0);
        }
    }
    return cfg;
}

// ── Helpers ──

void seed_book(MatchingEngine& engine, const std::string& symbol, Price mid) {
    // 10 levels each side, 5 orders per level
    // this gives a reasonable starting book so the first few market orders
    // don't just sail through into the void
    OrderId id = 1;
    for (int lvl = 1; lvl <= 10; ++lvl) {
        for (int j = 0; j < 5; ++j) {
            NewOrderRequest bid{};
            bid.id = id++;
            bid.side = Side::Buy;
            bid.type = OrderType::Limit;
            bid.tif = TimeInForce::GTC;
            bid.price = mid - lvl;
            bid.quantity = 100 + (j * 50);
            std::strncpy(bid.symbol, symbol.c_str(), 15);
            engine.submit_order(bid);

            NewOrderRequest ask{};
            ask.id = id++;
            ask.side = Side::Sell;
            ask.type = OrderType::Limit;
            ask.tif = TimeInForce::GTC;
            ask.price = mid + lvl;
            ask.quantity = 100 + (j * 50);
            std::strncpy(ask.symbol, symbol.c_str(), 15);
            engine.submit_order(ask);
        }
    }
}

void write_trades_csv(const std::string& path, const std::vector<Trade>& trades) {
    std::ofstream ofs(path);
    ofs << "seq,buy_id,sell_id,price,qty,aggressor\n";
    for (const auto& t : trades) {
        ofs << t.sequence << ","
            << t.buy_order_id << ","
            << t.sell_order_id << ","
            << t.price << ","
            << t.quantity << ","
            << (t.aggressor == Side::Buy ? "B" : "S") << "\n";
    }
}

void write_midprices_csv(const std::string& path, const std::vector<Price>& mids) {
    std::ofstream ofs(path);
    ofs << "idx,midprice\n";
    for (size_t i = 0; i < mids.size(); ++i) {
        ofs << i << "," << mids[i] << "\n";
    }
}

void write_spreads_csv(const std::string& path, const std::vector<Price>& spreads) {
    std::ofstream ofs(path);
    ofs << "idx,quoted_spread\n";
    for (size_t i = 0; i < spreads.size(); ++i) {
        ofs << i << "," << spreads[i] << "\n";
    }
}

// ── Main ──

int main(int argc, char* argv[]) {
    auto cfg = parse_args(argc, argv);

    std::cout << "\n";
    std::cout << "  ╔══════════════════════════════════════════╗\n";
    std::cout << "  ║       MicroExchange v1.0.0               ║\n";
    std::cout << "  ║   CLOB + Market Data + Analytics         ║\n";
    std::cout << "  ╚══════════════════════════════════════════╝\n\n";

    std::cout << "  Symbol:   " << cfg.symbol << "\n";
    std::cout << "  Duration: " << cfg.duration << " sec\n";
    std::cout << "  Init mid: " << cfg.init_mid << " ($"
              << std::fixed << std::setprecision(2) << cfg.init_mid / 100.0 << ")\n";
    std::cout << "  Agents:   " << cfg.n_agents << "\n\n";

    fs::create_directories(cfg.out_dir);

    auto wall_start = std::chrono::high_resolution_clock::now();

    // ── Engine setup ──
    MatchingEngine engine;
    engine.add_symbol(cfg.symbol);
    auto* book = engine.get_book(cfg.symbol);

    // The OrderBook now supports multi-listener fan-out, so attaching the
    // feed publisher no longer clobbers the engine's internal trade routing.
    FeedPublisher feed;
    feed.attach(*book);

    // ── Agents ──
    std::vector<ZIAgent> agents;
    for (size_t i = 0; i < cfg.n_agents; ++i) {
        ZIAgent::Parameters p;
        p.agent_id = i;
        // tighter placement = more crossing = more trades
        p.sigma_price = 3.0 + (i % 3) * 1.5;
        p.market_order_prob = 0.15 + (i % 4) * 0.02;
        p.mean_size = 150.0;
        p.sigma_size = 0.5;
        agents.emplace_back(p, 42 + i);
    }

    seed_book(engine, cfg.symbol, cfg.init_mid);

    // ── Generate events ──
    HawkesProcess::Parameters hp;
    hp.mu = 50.0;
    hp.alpha = 35.0;
    hp.beta = 50.0;
    HawkesProcess hawkes(hp, 12345);
    auto events = hawkes.generate_sided(cfg.duration);

    std::cout << "  [1/4] Generated " << events.size() << " events (Hawkes n="
              << std::fixed << std::setprecision(2) << hp.alpha / hp.beta << ")\n";

    // ── Run matching ──
    std::vector<Trade> trades;
    std::vector<double> trade_times;       // simulated wall-clock seconds
    trades.reserve(events.size() / 3);
    trade_times.reserve(events.size() / 3);

    std::vector<Price> midprices;
    std::vector<Price> spreads;
    std::vector<double> mid_times;
    midprices.reserve(events.size());
    spreads.reserve(events.size());
    mid_times.reserve(events.size());

    double current_event_time = 0.0;       // updated in the main loop
    engine.set_trade_callback([&](const Trade& t) {
        trades.push_back(t);
        trade_times.push_back(current_event_time);
    });

    OrderId next_id = 10000;
    size_t progress_step = events.size() / 10;

    for (size_t i = 0; i < events.size(); ++i) {
        if (progress_step > 0 && i % progress_step == 0 && i > 0) {
            std::cout << "  [2/4] Processing... "
                      << (i * 100 / events.size()) << "%\r" << std::flush;
        }

        current_event_time = events[i].timestamp;

        auto mid = book->midprice().value_or(cfg.init_mid);
        auto sprd = book->spread().value_or(2);
        midprices.push_back(mid);
        spreads.push_back(sprd);
        mid_times.push_back(current_event_time);

        size_t agent_idx = next_id % cfg.n_agents;
        auto req = agents[agent_idx].generate_order(
            mid, sprd, events[i].is_buy, next_id++, cfg.symbol.c_str());
        engine.submit_order(req);
    }

    std::cout << "  [2/4] Matching complete: " << trades.size() << " trades from "
              << events.size() << " orders\n";

    // ── Analytics ──
    std::cout << "  [3/4] Computing analytics...\n";

    // Helper: lookup mid at a given simulated time using sorted mid_times
    auto mid_at_time = [&](double t) -> Price {
        if (mid_times.empty()) return cfg.init_mid;
        auto it = std::lower_bound(mid_times.begin(), mid_times.end(), t);
        if (it == mid_times.begin()) return midprices.front();
        if (it == mid_times.end())   return midprices.back();
        size_t idx = static_cast<size_t>(std::distance(mid_times.begin(), it));
        return midprices[idx];
    };

    // Spread decomposition (Huang-Stoll). 5-second post-trade reversion window.
    SpreadAnalyzer spread_analyzer;
    std::vector<SpreadAnalyzer::TradeInput> spread_inputs;

    for (size_t i = 0; i < trades.size(); ++i) {
        SpreadAnalyzer::TradeInput ti;
        ti.trade_price = trades[i].price;
        ti.mid_before  = mid_at_time(trade_times[i]);
        ti.mid_after   = mid_at_time(trade_times[i] + 5.0);
        ti.volume      = trades[i].quantity;
        ti.aggressor   = trades[i].aggressor;
        spread_inputs.push_back(ti);
    }

    auto spread_result = spread_analyzer.compute(spread_inputs, spreads);

    // Kyle's lambda — properly time-indexed now
    ImpactAnalyzer impact_analyzer;
    std::vector<ImpactAnalyzer::TradeInput> impact_inputs;
    std::vector<std::pair<double, Price>> timed_mids;
    impact_inputs.reserve(trades.size());
    timed_mids.reserve(midprices.size());

    for (size_t i = 0; i < trades.size(); ++i) {
        ImpactAnalyzer::TradeInput ti;
        ti.timestamp = trade_times[i];
        ti.price     = trades[i].price;
        ti.volume    = trades[i].quantity;
        ti.aggressor = trades[i].aggressor;
        impact_inputs.push_back(ti);
    }
    for (size_t i = 0; i < midprices.size(); ++i) {
        timed_mids.push_back({mid_times[i], midprices[i]});
    }

    auto kyle_result = impact_analyzer.estimate_kyle_lambda(impact_inputs, timed_mids, 5.0);

    // Stylized facts
    StylizedFacts stylized;
    auto facts = stylized.compute(midprices);

    // ── Output ──
    std::cout << "  [4/4] Writing output to " << cfg.out_dir << "/\n\n";

    write_trades_csv(cfg.out_dir + "/trades.csv", trades);
    write_midprices_csv(cfg.out_dir + "/midprices.csv", midprices);
    write_spreads_csv(cfg.out_dir + "/spreads.csv", spreads);

    // Summary report
    {
        std::ofstream rpt(cfg.out_dir + "/report.txt");
        auto also = [&](auto& os, const std::string& line) {
            os << line << "\n";
            std::cout << line << "\n";
        };

        auto wall_end = std::chrono::high_resolution_clock::now();
        double wall_sec = std::chrono::duration<double>(wall_end - wall_start).count();

        also(rpt, "  ═══════════════════════════════════════════");
        also(rpt, "  MicroExchange — Simulation Report");
        also(rpt, "  ═══════════════════════════════════════════");
        also(rpt, "");
        also(rpt, "  Engine Statistics");
        also(rpt, "  ─────────────────────────────────────────");

        auto stats = engine.get_stats();
        also(rpt, "  Total orders:    " + std::to_string(stats.total_orders));
        also(rpt, "  Total trades:    " + std::to_string(stats.total_trades));
        also(rpt, "  Total volume:    " + std::to_string(stats.total_volume));
        also(rpt, "  Active orders:   " + std::to_string(stats.active_orders));

        auto fstats = feed.get_stats();
        also(rpt, "  Feed messages:   " + std::to_string(fstats.total_messages)
                  + " (A=" + std::to_string(fstats.add_count)
                  + " T=" + std::to_string(fstats.trade_count)
                  + " D=" + std::to_string(fstats.delete_count)
                  + " Q=" + std::to_string(fstats.quote_count) + ")");

        {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2) << wall_sec;
            also(rpt, "  Wall time:       " + oss.str() + " sec");
        }
        {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(0) << events.size() / wall_sec;
            also(rpt, "  Throughput:      " + oss.str() + " events/sec");
        }

        also(rpt, "");
        also(rpt, "  Spread Decomposition (Huang-Stoll)");
        also(rpt, "  ─────────────────────────────────────────");

        auto fmt = [](double v) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2) << v;
            return oss.str();
        };

        also(rpt, "  Quoted spread:      " + fmt(spread_result.avg_quoted_spread) + " ticks");
        also(rpt, "  Effective spread:   " + fmt(spread_result.avg_effective_spread) + " ticks");
        also(rpt, "  Realized spread:    " + fmt(spread_result.avg_realized_spread) + " ticks");
        also(rpt, "  Price impact:       " + fmt(spread_result.avg_price_impact) + " ticks");
        also(rpt, "  Adverse selection:  " + fmt(spread_result.adverse_selection_pct) + "%");

        also(rpt, "");
        also(rpt, "  Kyle's Lambda");
        also(rpt, "  ─────────────────────────────────────────");
        {
            std::ostringstream oss;
            oss << std::scientific << std::setprecision(3) << kyle_result.lambda;
            also(rpt, "  lambda:   " + oss.str() + " (ticks per share, signed)");
        }
        also(rpt, "  R²:       " + fmt(kyle_result.r_squared));

        {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << kyle_result.t_statistic;
            also(rpt, "  t-stat:   " + oss.str());
        }

        also(rpt, "  N:        " + std::to_string(kyle_result.num_intervals));

        also(rpt, "");
        also(rpt, "  Stylized Facts");
        also(rpt, "  ─────────────────────────────────────────");
        also(rpt, "  Excess kurtosis:     " + fmt(facts.return_kurtosis));
        also(rpt, "  AC(|r|, lag=1):      " + fmt(facts.abs_return_ac_lag1));
        also(rpt, "  AC(|r|, lag=5):      " + fmt(facts.abs_return_ac_lag5));
        also(rpt, "  AC(|r|, lag=10):     " + fmt(facts.abs_return_ac_lag10));

        also(rpt, "");
        for (const auto& fc : facts.fact_checks) {
            std::string status = fc.reproduced ? "  ✓ " : "  ✗ ";
            also(rpt, status + fc.name + " → " + fmt(fc.value) + " (benchmark: " + fc.benchmark + ")");
        }

        also(rpt, "");
        also(rpt, "  ═══════════════════════════════════════════");

        also(rpt, "");
        also(rpt, "  Output files:");
        also(rpt, "    " + cfg.out_dir + "/trades.csv");
        also(rpt, "    " + cfg.out_dir + "/midprices.csv");
        also(rpt, "    " + cfg.out_dir + "/spreads.csv");
        also(rpt, "    " + cfg.out_dir + "/report.txt");
        also(rpt, "");
    }

    return 0;
}
