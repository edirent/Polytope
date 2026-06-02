#pragma once

#include "engine/strategy/market_making/public/QuoteIntent.h"

#include <cstdint>

namespace trading_engine::strategy::market_making {

struct ActiveQuoteState {
    std::uint64_t quote_group_id = 0;
    std::uint64_t quote_intent_id = 0;
    std::uint32_t asset_index = 0;
    QuoteStatus status = QuoteStatus::Created;

    bool has_bid = false;
    bool has_ask = false;
    QuoteLeg bid;
    QuoteLeg ask;

    std::int64_t filled_bid_qty_lots = 0;
    std::int64_t filled_ask_qty_lots = 0;

    std::int64_t fair_value_tick = 0;
    std::int64_t current_position_lots = 0;

    std::uint64_t created_ts_ns = 0;
    std::uint64_t expires_at_ns = 0;
    std::uint64_t snapshot_version_hash = 0;
    std::uint64_t idempotency_hash = 0;
};

[[nodiscard]] inline ActiveQuoteState active_quote_from_intent(
    const QuoteIntent& intent,
    QuoteStatus status = QuoteStatus::ActivePaper
) {
    ActiveQuoteState state;
    state.quote_group_id = intent.quote_group_id;
    state.quote_intent_id = intent.quote_intent_id;
    state.asset_index = intent.asset_index;
    state.status = status;
    state.has_bid = intent.has_bid;
    state.has_ask = intent.has_ask;
    state.bid = intent.bid;
    state.ask = intent.ask;
    state.fair_value_tick = intent.fair_value_tick;
    state.current_position_lots = intent.current_position_lots;
    state.created_ts_ns = intent.created_ts_ns;
    state.expires_at_ns = intent.expires_at_ns;
    state.snapshot_version_hash = intent.snapshot_version_hash;
    state.idempotency_hash = intent.idempotency_hash;
    return state;
}

}  // namespace trading_engine::strategy::market_making
