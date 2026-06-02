#pragma once

#include "engine/strategy/market_making/public/MarketMakingTypes.h"

#include <cstdint>

namespace trading_engine::strategy::market_making {

struct CancelQuoteIntent {
    std::uint64_t cancel_intent_id = 0;
    std::uint64_t quote_group_id = 0;
    std::uint64_t active_quote_id = 0;
    std::uint32_t asset_index = 0;
    CancelReason reason = CancelReason::None;
    std::uint64_t created_ts_ns = 0;
    std::uint64_t idempotency_hash = 0;
};

[[nodiscard]] std::uint64_t compute_cancel_quote_intent_hash(
    const CancelQuoteIntent& cancel
) noexcept;

}  // namespace trading_engine::strategy::market_making
