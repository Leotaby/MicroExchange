#pragma once

#include "Order.h"
#include <vector>
#include <numeric>
#include <cmath>
#include <algorithm>

namespace micro_exchange::analytics {

using namespace micro_exchange::core;

/**
 * SpreadAnalyzer — Spread decomposition following Huang & Stoll (1997).
 *
 * Theory:
 * ───────
 * The bid-ask spread compensates market makers for three costs:
 *
 *   1. Order processing costs (fixed costs of operating)
 *   2. Inventory holding costs (risk of holding unbalanced position)
 *   3. Adverse selection costs (trading against informed counterparties)
 *
 * We decompose using:
 *
 *   Quoted Spread:    S_q = Ask - Bid
 *   Effective Spread: S_e = 2 · d · (P_trade - M_t)
 *                     where d = +1 for buys, -1 for sells, M = midpoint
 *   Realized Spread:  S_r = 2 · d · (P_trade - M_{t+Δ})
 *                     captures market maker revenue after price moves
 *   Price Impact:     PI = S_e - S_r = 2 · d · (M_{t+Δ} - M_t)
 *                     permanent information content of the trade
 *
 *   Adverse Selection % = PI / S_e
 *
 * The realized spread is the market maker's actual profit per trade.
 * A high adverse selection ratio (>50%) means the spread mostly
 * compensates for trading against informed flow, not order processing.
 *
 * Typical values (US equities, 2020s):
 *   Effective spread: 1-5 bps for large-cap
 *   Adverse selection: 40-70%
 *   Realized spread: 30-60% of effective
 */
class SpreadAnalyzer {
public:
    struct TradeInput {
        Price    trade_price;
        Price    mid_before;     // Midpoint at trade time
        Price    mid_after;      // Midpoint Δ seconds later (typically 5s)
        Quantity volume;
        Side     aggressor;      // Buy-initiated or sell-initiated
    };

    struct SpreadMetrics {
        double avg_quoted_spread;      // Average quoted spread (ticks)
        double avg_effective_spread;   // Average effective spread (ticks)
        double avg_realized_spread;    // Average realized spread (ticks)
        double avg_price_impact;       // Average price impact (ticks)
        double adverse_selection_pct;  // Price impact / effective spread

        // Distributions
        double median_effective_spread;
        double p95_effective_spread;

        // Volume-weighted versions
        double vwap_effective_spread;
        double vwap_realized_spread;

        size_t num_trades;
    };

    /**
     * Compute full spread decomposition.
     *
     * @param trades         Trade records with pre/post midpoints
     * @param quoted_spreads Raw quoted spreads at each event
     */
    SpreadMetrics compute(const std::vector<TradeInput>& trades,
                          const std::vector<Price>& quoted_spreads) const {
        SpreadMetrics result{};
        if (trades.empty()) return result;

        result.num_trades = trades.size();

        // ── Quoted spread ──
        if (!quoted_spreads.empty()) {
            double sum = std::accumulate(quoted_spreads.begin(), quoted_spreads.end(), 0.0);
            result.avg_quoted_spread = sum / quoted_spreads.size();
        }

        // ── Effective, realized, impact spreads ──
        std::vector<double> effective_spreads;
        effective_spreads.reserve(trades.size());

        double sum_effective = 0, sum_realized = 0, sum_impact = 0;
        double vw_effective = 0, vw_realized = 0;
        Quantity total_volume = 0;

        for (const auto& t : trades) {
            double d = (t.aggressor == Side::Buy) ? 1.0 : -1.0;

            double eff = 2.0 * d * (t.trade_price - t.mid_before);
            double real = 2.0 * d * (t.trade_price - t.mid_after);
            double impact = eff - real;  // = 2 * d * (mid_after - mid_before)

            sum_effective += std::abs(eff);
            sum_realized += real;      // Can be negative (market maker loses)
            sum_impact += std::abs(impact);

            effective_spreads.push_back(std::abs(eff));

            vw_effective += std::abs(eff) * t.volume;
            vw_realized += real * t.volume;
            total_volume += t.volume;
        }

        result.avg_effective_spread = sum_effective / trades.size();
        result.avg_realized_spread = sum_realized / trades.size();
        result.avg_price_impact = sum_impact / trades.size();

        if (result.avg_effective_spread > 0) {
            result.adverse_selection_pct =
                (result.avg_price_impact / result.avg_effective_spread) * 100.0;
        }

        // Volume-weighted
        if (total_volume > 0) {
            result.vwap_effective_spread = vw_effective / total_volume;
            result.vwap_realized_spread = vw_realized / total_volume;
        }

        // Percentiles
        std::sort(effective_spreads.begin(), effective_spreads.end());
        result.median_effective_spread = percentile(effective_spreads, 0.5);
        result.p95_effective_spread = percentile(effective_spreads, 0.95);

        return result;
    }

private:
    static double percentile(const std::vector<double>& sorted, double p) {
        if (sorted.empty()) return 0;
        double idx = p * (sorted.size() - 1);
        size_t lo = static_cast<size_t>(idx);
        size_t hi = std::min(lo + 1, sorted.size() - 1);
        double frac = idx - lo;
        return sorted[lo] * (1 - frac) + sorted[hi] * frac;
    }
};

} // namespace micro_exchange::analytics
