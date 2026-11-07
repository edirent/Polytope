#pragma once

#include "engine/strategy/market_making/core/MarketMakingWorkflow.h"
#include "engine/strategy/market_making/products/up_down_5m/UpDown5mFairProvider.h"

#include <cstdint>
#include <string>

namespace trading_engine::strategy::market_making::products::up_down_5m {

struct UpDown5mQuoteControllerConfig {
    bool enabled = true;

    std::uint64_t min_quote_tau_ms = 30'000;
    std::uint64_t hard_cancel_tau_ms = 20'000;

    std::uint64_t max_oracle_age_ms = 300;
    std::int64_t max_book_age_ns = 500'000'000;

    std::int64_t base_half_spread_tick = 15'000;
    std::int64_t min_half_spread_tick = 10'000;
    std::int64_t max_half_spread_tick = 200'000;
    std::uint64_t gamma_latency_floor_ms = 100;
    std::int64_t gamma_buffer_multiplier_bps = 10'000;

    std::int64_t z_gate_tau_gt_180_bps = 10'000;
    std::int64_t z_gate_tau_60_180_bps = 7'000;
    std::int64_t z_gate_tau_30_60_bps = 4'000;
    std::int64_t losing_side_hard_cutoff_bps = 15'000;

    std::int64_t base_max_canonical_lots = 100;
    bool allow_directional_inventory = false;
    std::int64_t directional_soft_cap_lots = 10;
    std::int64_t directional_edge_scale_tick = 50'000;
    std::int64_t max_inventory_skew_tick = 75'000;

    std::uint64_t ttl_ms_tau_gt_180 = 1'500;
    std::uint64_t ttl_ms_tau_60_180 = 750;
    std::uint64_t ttl_ms_tau_30_60 = 300;
    std::int64_t fair_move_tick_tau_gt_180 = 2;
    std::int64_t fair_move_tick_tau_lt_180 = 1;
};

struct UpDown5mQuotePlan {
    bool ok = false;
    bool opening_allowed = false;
    bool cancel_all_quotes = false;
    bool disable_bid_quotes = false;
    bool disable_ask_quotes = false;

    std::int64_t external_fair_value_tick = 0;
    std::int64_t canonical_yes_fair_value_tick = 0;
    std::int64_t dynamic_target_position_lots = 0;
    std::int64_t dynamic_min_inventory_lots = 0;
    std::int64_t dynamic_max_inventory_lots = 0;
    std::int64_t dynamic_max_inventory_skew_tick = 0;
    std::int64_t dynamic_half_spread_tick = 0;
    std::uint64_t dynamic_quote_ttl_ns = 0;
    std::int64_t dynamic_requote_threshold_tick = 0;
    std::int64_t dynamic_max_book_age_ns = 0;

    ProductDirection asset_direction = ProductDirection::Unknown;
    ProductDirection winning_direction = ProductDirection::Unknown;
    std::int64_t z_score_bps = 0;
    std::uint64_t tau_ms = 0;
    std::int64_t dynamic_inventory_cap_lots = 0;
    bool settlement_source_ok = false;
    bool reward_size_lift_allowed = false;
    bool reward_spread_compression_allowed = false;

    std::int64_t current_canonical_yes_position_lots = 0;
    std::int64_t target_canonical_yes_lots = 0;

    std::string reason;

    void apply_to(MarketMakingInput* input) const noexcept;
};

class UpDown5mQuoteController {
public:
    explicit UpDown5mQuoteController(
        UpDown5mQuoteControllerConfig config = {}
    );

    [[nodiscard]] UpDown5mQuotePlan build_plan(
        const UpDown5mMarketContext& context,
        std::int64_t current_asset_position_lots
    ) const;

private:
    UpDown5mQuoteControllerConfig config_;
    UpDown5mFairProvider fair_provider_;
};

}  // namespace trading_engine::strategy::market_making::products::up_down_5m
