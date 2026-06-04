#pragma once

#include "engine/strategy/market_making/public/MarketMakingTypes.h"

#include <cstdint>

namespace trading_engine::strategy::market_making {

struct DigitalOptionFairInput {
    double spot = 0.0;
    double strike = 0.0;
    double vol_annual_bps = 0.0;
    double drift_annual_bps = 0.0;
    std::uint64_t time_to_expiry_ns = 0;
    bool invert = false;
};

struct DigitalOptionFairResult {
    bool ok = false;
    double probability = 0.0;
    std::int64_t fair_value_tick = 0;
    double d2 = 0.0;
    double pdf_d2 = 0.0;
    double time_to_expiry_years = 0.0;
    double probability_variance_to_expiry = 0.0;
};

struct AvellanedaStoikovInput {
    DigitalOptionFairResult fair;
    double risk_aversion = 0.0;
    double order_arrival_k = 0.0;
    double spread_multiplier = 1.0;
    std::int64_t min_half_spread_tick = 0;
    std::int64_t max_half_spread_tick = 0;
    std::int64_t min_inventory_skew_tick = 0;
    std::int64_t max_inventory_skew_tick = 0;
};

struct AvellanedaStoikovResult {
    bool ok = false;
    std::int64_t half_spread_tick = 0;
    std::int64_t max_inventory_skew_tick = 0;
    std::int64_t reservation_risk_tick = 0;
};

class DigitalOptionFairModel {
public:
    [[nodiscard]] DigitalOptionFairResult compute(
        const DigitalOptionFairInput& input
    ) const noexcept;
};

class AvellanedaStoikovModel {
public:
    [[nodiscard]] AvellanedaStoikovResult compute(
        const AvellanedaStoikovInput& input
    ) const noexcept;
};

}  // namespace trading_engine::strategy::market_making
