#pragma once

#include "engine/strategy/market_making/public/MarketMakingTypes.h"

#include <cstdint>
#include <string>

namespace trading_engine::strategy::market_making {

struct QuoteLeg {
    std::string market_id;
    std::string asset_id;

    std::uint32_t market_index = 0;
    std::uint32_t asset_index = 0;

    QuoteSide side = QuoteSide::Bid;

    std::int64_t price_tick = 0;
    std::int64_t quantity_lots = 0;

    std::int64_t fair_value_tick = 0;
    std::int64_t edge_to_fair_tick = 0;
    bool risk_reducing = false;
    bool allow_fair_deviation_exemption = false;
    bool allow_spread_exemption = false;

    std::uint64_t book_version = 0;
    std::uint64_t snapshot_version_hash = 0;
};

struct QuoteIntent {
    std::uint64_t quote_intent_id = 0;
    QuoteIntentType type = QuoteIntentType::None;

    std::uint64_t strategy_id = 0;
    std::uint64_t quote_group_id = 0;

    std::string market_id;
    std::string asset_id;
    std::uint32_t market_index = 0;
    std::uint32_t asset_index = 0;
    QuoteIntentRiskMode risk_mode = QuoteIntentRiskMode::Opening;

    bool has_bid = false;
    bool has_ask = false;
    QuoteLeg bid;
    QuoteLeg ask;

    std::int64_t fair_value_tick = 0;
    std::int64_t half_spread_tick = 0;
    std::int64_t inventory_skew_tick = 0;
    std::int64_t target_position_lots = 0;
    std::int64_t current_position_lots = 0;

    bool reward_config_present = false;
    bool reward_eligible = false;
    std::string reward_condition_id;
    std::int64_t reward_max_spread_tick = 0;
    std::int64_t reward_min_size_lots = 0;
    std::string reward_reason;

    std::uint64_t created_ts_ns = 0;
    std::uint64_t expires_at_ns = 0;

    std::uint64_t snapshot_version_hash = 0;
    std::uint64_t oracle_artifact_hash = 0;
    std::uint64_t policy_hash = 0;
    std::uint64_t idempotency_hash = 0;

    std::string reason;
};

[[nodiscard]] std::uint64_t compute_quote_leg_hash(const QuoteLeg& leg) noexcept;
[[nodiscard]] std::uint64_t compute_quote_intent_hash(
    const QuoteIntent& intent
) noexcept;

}  // namespace trading_engine::strategy::market_making
