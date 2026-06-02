#pragma once

#include "engine/strategy/market_making/public/CancelQuoteIntent.h"
#include "engine/strategy/market_making/state/ActiveQuoteState.h"

namespace trading_engine::strategy::market_making {

[[nodiscard]] inline CancelQuoteIntent make_cancel_intent(
    const ActiveQuoteState& active,
    CancelReason reason,
    std::uint64_t now_ns
) noexcept {
    CancelQuoteIntent cancel;
    cancel.quote_group_id = active.quote_group_id;
    cancel.active_quote_id = active.quote_intent_id;
    cancel.asset_index = active.asset_index;
    cancel.reason = reason;
    cancel.created_ts_ns = now_ns;
    cancel.idempotency_hash = compute_cancel_quote_intent_hash(cancel);
    cancel.cancel_intent_id = cancel.idempotency_hash;
    return cancel;
}

}  // namespace trading_engine::strategy::market_making
