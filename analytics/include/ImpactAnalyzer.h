#pragma once

#include "Order.h"
#include <vector>
#include <numeric>
#include <cmath>
#include <algorithm>
#include <cassert>

namespace micro_exchange::analytics {

using namespace micro_exchange::core;

/**
 * ImpactAnalyzer — Price impact measurement and Kyle's lambda estimation.
 *
 * Theory:
 * ───────
 * Kyle (1985) established the fundamental model of informed trading:
 *
 *   ΔP = λ · ΔX + ε
 *
 * Where:
 *   ΔP = price change over interval
 *   ΔX = net signed order flow (buy volume - sell volume)
 *   λ  = Kyle's lambda (price impact coefficient)
 *   ε  = noise
 *
 * λ measures the market's "price impact per unit of order flow."
 * Higher λ means:
 *   • Less liquid market
 *   • More information in order flow
 *   • Wider effective spreads
 *
 * Kyle showed that in equilibrium, λ = σ_v / (2 · σ_u)
 * where σ_v = volatility of fundamental value, σ_u = noise trader volume.
 *
 * We estimate λ using OLS on aggregated intervals:
 *   1. Divide trading day into N intervals (e.g., 5-second buckets)
 *   2. Compute ΔP and ΔX for each interval
 *   3. Run regression ΔP = α + λ · ΔX + ε
 *
 * Additional impact metrics:
 *   • Temporary impact: price move that reverts (inventory/bounce)
 *   • Permanent impact: price move that persists (information)
 *   • Impact curve: impact as function of trade size quantile
 */
class ImpactAnalyzer {
public:
    struct TradeInput {
        double   timestamp;      // Seconds since start
        Price    price;
        Quantity volume;
        Side     aggressor;
    };

    struct KyleLambdaResult {
        double lambda;           // Price impact coefficient
        double alpha;            // Intercept
        double r_squared;        // Goodness of fit
        double t_statistic;      // Statistical significance of lambda
        double std_error;        // Standard error of lambda
        size_t num_intervals;    // Number of intervals used
    };

    struct ImpactCurvePoint {
        double volume_quantile;  // 0-100 percentile
        double avg_impact;       // Average absolute price impact
    };

    /**
     * Estimate Kyle's lambda via OLS regression.
     *
     * @param trades      Vector of trade records
     * @param midprices   Midprice time series
     * @param interval_sec  Aggregation interval (seconds)
     */
    KyleLambdaResult estimate_kyle_lambda(
        const std::vector<TradeInput>& trades,
        const std::vector<std::pair<double, Price>>& timed_midprices,
        double interval_sec = 5.0) const
    {
        if (trades.empty() || timed_midprices.empty()) {
            return {};
        }

        // ── Aggregate into intervals ──
        double max_time = trades.back().timestamp;
        size_t num_intervals = static_cast<size_t>(max_time / interval_sec) + 1;

        // ΔX: signed order flow per interval
        std::vector<double> delta_x(num_intervals, 0.0);
        // ΔP: price change per interval
        std::vector<double> delta_p(num_intervals, 0.0);

        // Accumulate signed volume
        for (const auto& t : trades) {
            size_t bucket = static_cast<size_t>(t.timestamp / interval_sec);
            if (bucket >= num_intervals) bucket = num_intervals - 1;

            double signed_vol = static_cast<double>(t.volume);
            if (t.aggressor == Side::Sell) signed_vol = -signed_vol;
            delta_x[bucket] += signed_vol;
        }

        // Compute price changes between interval boundaries
        for (size_t i = 1; i < num_intervals; ++i) {
            double t_start = (i - 1) * interval_sec;
            double t_end   = i * interval_sec;

            Price p_start = find_nearest_mid(timed_midprices, t_start);
            Price p_end   = find_nearest_mid(timed_midprices, t_end);

            delta_p[i] = static_cast<double>(p_end - p_start);
        }

        // ── OLS Regression: ΔP = α + λ · ΔX + ε ──
        // Skip first interval (no ΔP available)
        std::vector<double> x, y;
        for (size_t i = 1; i < num_intervals; ++i) {
            if (delta_x[i] != 0.0) {  // Skip empty intervals
                x.push_back(delta_x[i]);
                y.push_back(delta_p[i]);
            }
        }

        return ols_regression(x, y);
    }

    /**
     * Compute impact curve: average price impact by trade size quantile.
     */
    std::vector<ImpactCurvePoint> compute_impact_curve(
        const std::vector<TradeInput>& trades,
        const std::vector<Price>& midprices_before,
        const std::vector<Price>& midprices_after,
        size_t num_quantiles = 10) const
    {
        assert(trades.size() == midprices_before.size());
        assert(trades.size() == midprices_after.size());

        // Compute per-trade impact
        struct TradeImpact {
            Quantity volume;
            double   impact;
        };

        std::vector<TradeImpact> impacts;
        for (size_t i = 0; i < trades.size(); ++i) {
            double imp = std::abs(
                static_cast<double>(midprices_after[i] - midprices_before[i])
            );
            impacts.push_back({trades[i].volume, imp});
        }

        // Sort by volume
        std::sort(impacts.begin(), impacts.end(),
            [](const auto& a, const auto& b) { return a.volume < b.volume; });

        // Compute average impact per quantile
        std::vector<ImpactCurvePoint> curve;
        size_t per_bin = impacts.size() / num_quantiles;
        if (per_bin == 0) per_bin = 1;

        for (size_t q = 0; q < num_quantiles; ++q) {
            size_t start = q * per_bin;
            size_t end = std::min((q + 1) * per_bin, impacts.size());
            if (start >= impacts.size()) break;

            double sum = 0;
            for (size_t i = start; i < end; ++i) {
                sum += impacts[i].impact;
            }

            curve.push_back({
                .volume_quantile = (q + 0.5) * 100.0 / num_quantiles,
                .avg_impact = sum / (end - start)
            });
        }

        return curve;
    }

private:
    Price find_nearest_mid(const std::vector<std::pair<double, Price>>& mids, double t) const {
        // Binary search for nearest timestamp
        auto it = std::lower_bound(mids.begin(), mids.end(), t,
            [](const auto& p, double val) { return p.first < val; });

        if (it == mids.end()) return mids.back().second;
        if (it == mids.begin()) return it->second;

        auto prev = std::prev(it);
        return (t - prev->first < it->first - t) ? prev->second : it->second;
    }

    KyleLambdaResult ols_regression(const std::vector<double>& x,
                                     const std::vector<double>& y) const {
        KyleLambdaResult result{};
        size_t n = x.size();
        if (n < 3) return result;

        result.num_intervals = n;

        // Compute means
        double mean_x = std::accumulate(x.begin(), x.end(), 0.0) / n;
        double mean_y = std::accumulate(y.begin(), y.end(), 0.0) / n;

        // Compute slope (lambda) and intercept (alpha)
        double ss_xy = 0, ss_xx = 0, ss_yy = 0;
        for (size_t i = 0; i < n; ++i) {
            double dx = x[i] - mean_x;
            double dy = y[i] - mean_y;
            ss_xy += dx * dy;
            ss_xx += dx * dx;
            ss_yy += dy * dy;
        }

        if (ss_xx == 0) return result;

        result.lambda = ss_xy / ss_xx;
        result.alpha = mean_y - result.lambda * mean_x;

        // R²
        if (ss_yy > 0) {
            result.r_squared = (ss_xy * ss_xy) / (ss_xx * ss_yy);
        }

        // Standard error and t-statistic
        double sse = 0;
        for (size_t i = 0; i < n; ++i) {
            double residual = y[i] - result.alpha - result.lambda * x[i];
            sse += residual * residual;
        }
        double mse = sse / (n - 2);
        result.std_error = std::sqrt(mse / ss_xx);

        if (result.std_error > 0) {
            result.t_statistic = result.lambda / result.std_error;
        }

        return result;
    }
};

} // namespace micro_exchange::analytics
