#pragma once

#include <cstdint>
#include <vector>
#include <random>
#include <cmath>
#include <algorithm>

namespace micro_exchange::sim {

/**
 * HawkesProcess — Self-exciting point process for order arrival times.
 *
 * Theoretical background:
 * ───────────────────────
 * In real markets, order arrivals are NOT Poisson. They exhibit:
 *   • Clustering: bursts of activity (earnings, news, momentum)
 *   • Self-excitation: each event increases the probability of the next
 *   • Long-memory: the intensity function has slow decay
 *
 * The Hawkes process (Hawkes, 1971) captures this with an intensity:
 *
 *   λ(t) = μ + Σ_{t_i < t} α · exp(-β · (t - t_i))
 *
 * Where:
 *   μ (mu)    = baseline intensity (orders/sec in calm market)
 *   α (alpha) = jump size per event (excitation magnitude)
 *   β (beta)  = decay rate (how quickly excitation fades)
 *
 * The branching ratio n = α/β controls the clustering intensity:
 *   n < 1: stationary (required for stability)
 *   n → 0: approaches Poisson
 *   n → 1: heavy clustering (approaches criticality)
 *
 * Empirical calibration (Bacry et al., 2015):
 *   Equity markets: n ≈ 0.6-0.8
 *   FX: n ≈ 0.5-0.7
 *
 * This generates the realistic auto-correlated event times that produce
 * the stylized facts we verify: volatility clustering, fat tails in returns,
 * and time-varying spread behavior.
 *
 * Simulation algorithm: Ogata's thinning method (Ogata, 1981).
 */
class HawkesProcess {
public:
    struct Parameters {
        double mu    = 10.0;
        double alpha = 6.0;
        double beta  = 8.0;

        [[nodiscard]] double branching_ratio() const { return alpha / beta; }
        [[nodiscard]] bool is_stationary() const { return alpha < beta; }
    };

    explicit HawkesProcess(Parameters params, uint64_t seed = 42)
        : params_(params)
        , rng_(seed)
        , exp_dist_(1.0)
        , uniform_(0.0, 1.0)
    {
        if (!params_.is_stationary()) {
            // Force stationarity by capping alpha
            params_.alpha = params_.beta * 0.95;
        }
    }

    /**
     * Generate event times using Ogata's thinning algorithm.
     *
     * @param duration  Simulation duration in seconds
     * @return Vector of event timestamps (in seconds)
     */
    std::vector<double> generate(double duration) {
        std::vector<double> events;
        events.reserve(static_cast<size_t>(duration * params_.mu * 2));

        double t = 0.0;
        double intensity = params_.mu;

        while (t < duration) {
            // Upper bound on intensity (current value)
            double lambda_bar = intensity;

            // Generate candidate time (exponential with rate lambda_bar)
            double dt = exp_dist_(rng_) / lambda_bar;
            t += dt;

            if (t >= duration) break;

            // Decay existing intensity
            intensity = compute_intensity(t, events);

            // Accept/reject (thinning)
            if (uniform_(rng_) <= intensity / lambda_bar) {
                events.push_back(t);
                // Self-excitation: intensity jumps by alpha
                intensity += params_.alpha;
            }
        }

        return events;
    }

    /**
     * Generate clustered events on both buy and sell sides.
     * Buy-sell asymmetry creates order flow imbalance episodes.
     */
    struct SidedEvent {
        double timestamp;
        bool   is_buy;
    };

    std::vector<SidedEvent> generate_sided(double duration, double buy_bias = 0.5) {
        auto times = generate(duration);
        std::vector<SidedEvent> events;
        events.reserve(times.size());

        // Introduce autocorrelation in buy/sell direction
        // (models informed flow persistence)
        bool last_side = true;
        double persistence = 0.6;  // Probability of same-side follow-up

        for (double t : times) {
            bool is_buy;
            if (uniform_(rng_) < persistence) {
                is_buy = last_side;  // Follow previous direction
            } else {
                is_buy = uniform_(rng_) < buy_bias;
            }
            events.push_back({t, is_buy});
            last_side = is_buy;
        }

        return events;
    }

    [[nodiscard]] const Parameters& params() const { return params_; }

private:
    double compute_intensity(double t, const std::vector<double>& events) const {
        double intensity = params_.mu;

        // Sum over recent events (optimization: only look back a few decay times)
        double lookback = 5.0 / params_.beta;  // ~99.3% of excitation decayed

        for (auto it = events.rbegin(); it != events.rend(); ++it) {
            double dt = t - *it;
            if (dt > lookback) break;
            intensity += params_.alpha * std::exp(-params_.beta * dt);
        }

        return intensity;
    }

    Parameters params_;
    std::mt19937_64 rng_;
    std::exponential_distribution<double> exp_dist_;
    std::uniform_real_distribution<double> uniform_;
};

} // namespace micro_exchange::sim
