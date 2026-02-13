#pragma once

#include "Order.h"
#include <vector>
#include <numeric>
#include <cmath>
#include <algorithm>
#include <string>

namespace micro_exchange::analytics {

using namespace micro_exchange::core;

/**
 * StylizedFacts — Verification of emergent market properties.
 *
 * Background:
 * ───────────
 * "Stylized facts" are statistical regularities observed across virtually
 * all financial markets and time periods (Cont, 2001):
 *
 *   1. Fat tails: Return distributions have excess kurtosis (κ >> 3)
 *      Typical: κ ≈ 5-30 for daily, even higher for intraday
 *
 *   2. Volatility clustering: Large returns beget large returns.
 *      Measured by autocorrelation of |r| or r² at lag 1+.
 *      AC(|r|, lag=1) ≈ 0.15-0.40
 *
 *   3. Asymmetric volatility: Bad news increases vol more than good news
 *      (leverage effect). Correlation(r_t, σ²_{t+1}) < 0.
 *
 *   4. Volume-volatility correlation: High volume episodes have high vol.
 *      Correlation > 0.3 typically.
 *
 *   5. Spread dynamics: Spread widens during high volatility / imbalance
 *      and narrows during calm periods.
 *
 * A simulation that reproduces these facts demonstrates understanding
 * of the mechanisms that generate them (arrival clustering, adverse
 * selection, inventory effects).
 *
 * We compute these metrics from the simulation output and compare
 * against empirical benchmarks.
 */
class StylizedFacts {
public:
    struct FactMetrics {
        // Fat tails
        double return_kurtosis;       // Excess kurtosis (Normal = 0)
        double return_skewness;       // Skewness
        double jarque_bera_stat;      // JB test statistic

        // Volatility clustering
        double abs_return_ac_lag1;    // Autocorrelation of |returns| at lag 1
        double abs_return_ac_lag5;    // At lag 5
        double abs_return_ac_lag10;   // At lag 10
        double squared_return_ac_lag1;

        // Volume-volatility
        double volume_volatility_corr;

        // Spread dynamics
        double spread_vol_corr;       // Correlation(spread, volatility)
        double spread_imbalance_corr; // Correlation(spread, |imbalance|)

        // Summary: which stylized facts are reproduced
        struct FactCheck {
            std::string name;
            bool        reproduced;
            double      value;
            std::string benchmark;
        };
        std::vector<FactCheck> fact_checks;
    };

    /**
     * Compute all stylized fact metrics.
     *
     * @param midprices     Time series of midpoints
     * @param volumes       Volume per interval
     * @param spreads       Spread per interval
     * @param imbalances    Volume imbalance per interval
     */
    FactMetrics compute(
        const std::vector<Price>& midprices,
        const std::vector<Quantity>& volumes = {},
        const std::vector<Price>& spreads = {},
        const std::vector<double>& imbalances = {}) const
    {
        FactMetrics result{};

        // ── Compute returns ──
        std::vector<double> returns;
        for (size_t i = 1; i < midprices.size(); ++i) {
            if (midprices[i-1] > 0) {
                returns.push_back(
                    static_cast<double>(midprices[i] - midprices[i-1]) / midprices[i-1]
                );
            }
        }

        if (returns.size() < 20) return result;

        // ── Fat tails ──
        double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
        double var = 0, m3 = 0, m4 = 0;
        for (double r : returns) {
            double d = r - mean;
            var += d * d;
            m3 += d * d * d;
            m4 += d * d * d * d;
        }
        var /= returns.size();
        m3 /= returns.size();
        m4 /= returns.size();

        double std_dev = std::sqrt(var);
        if (std_dev > 0) {
            result.return_skewness = m3 / (std_dev * std_dev * std_dev);
            result.return_kurtosis = m4 / (var * var) - 3.0;  // Excess kurtosis
        }

        // Jarque-Bera
        double n = returns.size();
        result.jarque_bera_stat = (n / 6.0) *
            (result.return_skewness * result.return_skewness +
             0.25 * result.return_kurtosis * result.return_kurtosis);

        // ── Volatility clustering ──
        std::vector<double> abs_returns(returns.size());
        std::vector<double> sq_returns(returns.size());
        std::transform(returns.begin(), returns.end(), abs_returns.begin(),
            [](double r) { return std::abs(r); });
        std::transform(returns.begin(), returns.end(), sq_returns.begin(),
            [](double r) { return r * r; });

        result.abs_return_ac_lag1 = autocorrelation(abs_returns, 1);
        result.abs_return_ac_lag5 = autocorrelation(abs_returns, 5);
        result.abs_return_ac_lag10 = autocorrelation(abs_returns, 10);
        result.squared_return_ac_lag1 = autocorrelation(sq_returns, 1);

        // ── Volume-volatility correlation ──
        if (!volumes.empty() && volumes.size() >= returns.size()) {
            std::vector<double> vol_d(volumes.begin(),
                volumes.begin() + std::min(volumes.size(), abs_returns.size()));
            std::vector<double> abs_r(abs_returns.begin(),
                abs_returns.begin() + vol_d.size());
            result.volume_volatility_corr = correlation(
                std::vector<double>(vol_d.begin(), vol_d.end()), abs_r);
        }

        // ── Spread dynamics ──
        if (!spreads.empty() && spreads.size() >= returns.size()) {
            std::vector<double> sprd_d;
            for (size_t i = 0; i < std::min(spreads.size(), abs_returns.size()); ++i) {
                sprd_d.push_back(static_cast<double>(spreads[i]));
            }
            std::vector<double> abs_r(abs_returns.begin(),
                abs_returns.begin() + sprd_d.size());
            result.spread_vol_corr = correlation(sprd_d, abs_r);
        }

        if (!imbalances.empty() && imbalances.size() >= returns.size()) {
            std::vector<double> abs_imb;
            for (size_t i = 0; i < std::min(imbalances.size(), abs_returns.size()); ++i) {
                abs_imb.push_back(std::abs(imbalances[i]));
            }
            std::vector<double> sprd_d;
            for (size_t i = 0; i < abs_imb.size() && i < spreads.size(); ++i) {
                sprd_d.push_back(static_cast<double>(spreads[i]));
            }
            if (sprd_d.size() == abs_imb.size()) {
                result.spread_imbalance_corr = correlation(sprd_d, abs_imb);
            }
        }

        // ── Fact checks ──
        result.fact_checks = {
            {"Fat tails (kurtosis > 3)", result.return_kurtosis > 0,
             result.return_kurtosis, "> 0 (excess kurtosis)"},
            {"Volatility clustering (AC|r| lag1 > 0.1)", result.abs_return_ac_lag1 > 0.1,
             result.abs_return_ac_lag1, "0.15-0.40"},
            {"Slow AC decay (lag10 > 0)", result.abs_return_ac_lag10 > 0,
             result.abs_return_ac_lag10, "> 0"},
        };

        if (!volumes.empty()) {
            result.fact_checks.push_back(
                {"Volume-volatility correlation > 0.1",
                 result.volume_volatility_corr > 0.1,
                 result.volume_volatility_corr, "> 0.3 typical"});
        }

        if (!spreads.empty()) {
            result.fact_checks.push_back(
                {"Spread widens with volatility",
                 result.spread_vol_corr > 0,
                 result.spread_vol_corr, "> 0"});
        }

        return result;
    }

private:
    double autocorrelation(const std::vector<double>& x, size_t lag) const {
        if (x.size() <= lag) return 0;
        size_t n = x.size();
        double mean = std::accumulate(x.begin(), x.end(), 0.0) / n;

        double numerator = 0, denominator = 0;
        for (size_t i = 0; i < n; ++i) {
            denominator += (x[i] - mean) * (x[i] - mean);
            if (i >= lag) {
                numerator += (x[i] - mean) * (x[i - lag] - mean);
            }
        }

        return (denominator > 0) ? numerator / denominator : 0;
    }

    double correlation(const std::vector<double>& x, const std::vector<double>& y) const {
        size_t n = std::min(x.size(), y.size());
        if (n < 3) return 0;

        double mean_x = std::accumulate(x.begin(), x.begin() + n, 0.0) / n;
        double mean_y = std::accumulate(y.begin(), y.begin() + n, 0.0) / n;

        double ss_xy = 0, ss_xx = 0, ss_yy = 0;
        for (size_t i = 0; i < n; ++i) {
            double dx = x[i] - mean_x;
            double dy = y[i] - mean_y;
            ss_xy += dx * dy;
            ss_xx += dx * dx;
            ss_yy += dy * dy;
        }

        double denom = std::sqrt(ss_xx * ss_yy);
        return (denom > 0) ? ss_xy / denom : 0;
    }
};

} // namespace micro_exchange::analytics
