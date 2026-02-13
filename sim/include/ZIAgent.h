#pragma once

#include "../../core/include/Order.h"
#include <random>
#include <cmath>
#include <algorithm>
#include <vector>

namespace micro_exchange::sim {

using namespace micro_exchange::core;

/**
 * ZIAgent — Zero-Intelligence trader with strategic cancellations.
 *
 * Theoretical background:
 * ───────────────────────
 * Zero-intelligence (ZI) models (Gode & Sunder, 1993) show that many
 * market properties emerge from the mechanics of the double auction itself,
 * not from trader sophistication. However, pure ZI misses:
 *
 *   • Realistic spread formation (ZI spreads are too wide)
 *   • Volatility clustering (ZI returns are too thin-tailed)
 *   • Strategic cancellation (real traders pull stale quotes)
 *
 * Our ZI-C (ZI with cancels) variant adds:
 *   1. Price placement relative to midpoint (not uniform over all prices)
 *   2. Strategic cancellation: orders far from mid get cancelled faster
 *   3. Size variation: log-normal order sizes (empirical fact)
 *
 * This produces realistic spread behavior and, combined with the Hawkes
 * arrival process, generates the stylized facts we verify.
 *
 * Parameters calibrated to approximate equity LOB dynamics:
 *   • σ_price: spread of limit order placement around mid
 *   • cancel_distance_factor: how far from mid before cancel probability rises
 *   • mean_size: average order size (shares)
 */
class ZIAgent {
public:
    struct Parameters {
        double sigma_price       = 5.0;
        double market_order_prob = 0.15;
        double mean_size    = 100.0;
        double sigma_size   = 0.8;
        double cancel_base_prob     = 0.02;
        double cancel_distance_mult = 0.005;
        uint64_t agent_id = 0;

        Parameters() = default;
    };

    explicit ZIAgent(Parameters params, uint64_t seed = 42)
        : params_(params)
        , rng_(seed)
        , normal_(0.0, params.sigma_price)
        , uniform_(0.0, 1.0)
        , lognormal_(std::log(params.mean_size), params.sigma_size)
    {}

    /**
     * Generate a new order given current market state.
     *
     * @param mid_price     Current midpoint (in ticks)
     * @param spread        Current spread (in ticks)
     * @param is_buy        Side of this order
     * @param next_order_id ID to assign
     * @return NewOrderRequest ready for submission
     */
    NewOrderRequest generate_order(Price mid_price, [[maybe_unused]] Price spread,
                                    bool is_buy, OrderId next_order_id,
                                    const char* symbol) {
        NewOrderRequest req{};
        req.id = next_order_id;
        req.side = is_buy ? Side::Buy : Side::Sell;
        std::memcpy(req.symbol, symbol, std::min(strlen(symbol), size_t(16)));

        // Decide market vs limit
        if (uniform_(rng_) < params_.market_order_prob) {
            req.type = OrderType::Market;
            req.tif  = TimeInForce::IOC;
            req.price = PRICE_MARKET;
        } else {
            req.type = OrderType::Limit;
            req.tif  = TimeInForce::GTC;

            // Price placement: normal distribution around mid
            // Buy orders: biased below mid; Sell orders: biased above mid
            double offset = std::abs(normal_(rng_));
            if (is_buy) {
                req.price = mid_price - static_cast<Price>(offset);
            } else {
                req.price = mid_price + static_cast<Price>(offset);
            }

            // Ensure price is positive and on tick grid
            req.price = std::max(Price(1), req.price);
        }

        // Order size: log-normal distribution
        double raw_size = lognormal_(rng_);
        req.quantity = std::max(Quantity(1),
            static_cast<Quantity>(std::round(raw_size)));

        // Round to lot size (100 shares)
        req.quantity = ((req.quantity + 50) / 100) * 100;
        if (req.quantity == 0) req.quantity = 100;

        return req;
    }

    /**
     * Decide whether to cancel an existing order.
     * Probability increases with distance from current midpoint.
     *
     * @param order       The resting order to evaluate
     * @param mid_price   Current midpoint
     * @return true if should cancel
     */
    bool should_cancel(const Order& order, Price mid_price) {
        if (!order.is_active()) return false;

        Price distance = std::abs(order.price - mid_price);
        double cancel_prob = params_.cancel_base_prob
                           + params_.cancel_distance_mult * distance;

        return uniform_(rng_) < cancel_prob;
    }

    /**
     * Generate a batch of cancel decisions for a set of orders.
     * Returns order IDs that should be cancelled.
     */
    std::vector<OrderId> evaluate_cancels(
        const std::vector<std::pair<OrderId, Price>>& resting_orders,
        Price mid_price)
    {
        std::vector<OrderId> to_cancel;
        for (const auto& [id, price] : resting_orders) {
            Price distance = std::abs(price - mid_price);
            double cancel_prob = params_.cancel_base_prob
                               + params_.cancel_distance_mult * distance;
            if (uniform_(rng_) < cancel_prob) {
                to_cancel.push_back(id);
            }
        }
        return to_cancel;
    }

    [[nodiscard]] const Parameters& params() const { return params_; }

private:
    Parameters params_;
    std::mt19937_64 rng_;
    std::normal_distribution<double> normal_;
    std::uniform_real_distribution<double> uniform_;
    std::lognormal_distribution<double> lognormal_;
};

} // namespace micro_exchange::sim
