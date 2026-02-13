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

    // NOTE: we used to attach FeedPublisher here but it overrides the book's
    // trade callback which breaks the engine's internal trade routing.
    // TODO: fix this properly with a multi-callback dispatcher.
    // For now, feed publishing is disabled in the main simulation path.
    // FeedPublisher feed;
    // feed.attach(*book);

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
    trades.reserve(events.size() / 3);  // rough estimate

    std::vector<Price> midprices;
    std::vector<Price> spreads;
    midprices.reserve(events.size());
    spreads.reserve(events.size());

    engine.set_trade_callback([&](const Trade& t) {
        trades.push_back(t);
    });

    OrderId next_id = 10000;
    size_t progress_step = events.size() / 10;

    for (size_t i = 0; i < events.size(); ++i) {
        if (progress_step > 0 && i % progress_step == 0 && i > 0) {
            std::cout << "  [2/4] Processing... "
                      << (i * 100 / events.size()) << "%\r" << std::flush;
        }

        auto mid = book->midprice().value_or(cfg.init_mid);
        auto sprd = book->spread().value_or(2);
        midprices.push_back(mid);
        spreads.push_back(sprd);

        size_t agent_idx = next_id % cfg.n_agents;
        auto req = agents[agent_idx].generate_order(
            mid, sprd, events[i].is_buy, next_id++, cfg.symbol.c_str());
        engine.submit_order(req);
    }

    std::cout << "  [2/4] Matching complete: " << trades.size() << " trades from "
              << events.size() << " orders\n";

    // ── Analytics ──
    std::cout << "  [3/4] Computing analytics...\n";

    // Spread decomposition
    SpreadAnalyzer spread_analyzer;
    std::vector<SpreadAnalyzer::TradeInput> spread_inputs;

    for (size_t i = 0; i < trades.size() && i < midprices.size(); ++i) {
        // HACK: using index-based mid lookup, not timestamp. close enough
        // for simulation but would need proper time-matching for real data
        size_t mid_idx = std::min(i, midprices.size() - 1);
        size_t mid_after = std::min(i + 200, midprices.size() - 1);  // ~5s ahead

        SpreadAnalyzer::TradeInput ti;
        ti.trade_price = trades[i].price;
        ti.mid_before = midprices[mid_idx];
        ti.mid_after = midprices[mid_after];
        ti.volume = trades[i].quantity;
        ti.aggressor = trades[i].aggressor;
        spread_inputs.push_back(ti);
    }

    auto spread_result = spread_analyzer.compute(spread_inputs, spreads);

    // Kyle's lambda
    ImpactAnalyzer impact_analyzer;
    std::vector<ImpactAnalyzer::TradeInput> impact_inputs;
    std::vector<std::pair<double, Price>> timed_mids;

    for (size_t i = 0; i < trades.size(); ++i) {
        ImpactAnalyzer::TradeInput ti;
        ti.timestamp = static_cast<double>(i) / 40.0;  // approximate
        ti.price = trades[i].price;
        ti.volume = trades[i].quantity;
        ti.aggressor = trades[i].aggressor;
        impact_inputs.push_back(ti);
    }
    for (size_t i = 0; i < midprices.size(); ++i) {
        timed_mids.push_back({static_cast<double>(i) / 40.0, midprices[i]});
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
        also(rpt, "  lambda:   " + fmt(kyle_result.lambda));
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
