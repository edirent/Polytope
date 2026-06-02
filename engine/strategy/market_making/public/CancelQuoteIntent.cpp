#include "engine/strategy/market_making/public/CancelQuoteIntent.h"

namespace trading_engine::strategy::market_making {

std::uint64_t compute_cancel_quote_intent_hash(
    const CancelQuoteIntent& cancel
) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    hash = fnv1a_mix(hash, cancel.cancel_intent_id);
    hash = fnv1a_mix(hash, cancel.quote_group_id);
    hash = fnv1a_mix(hash, cancel.active_quote_id);
    hash = fnv1a_mix(hash, cancel.asset_index);
    hash = fnv1a_mix(hash, static_cast<std::uint8_t>(cancel.reason));
    hash = fnv1a_mix(hash, cancel.created_ts_ns);
    hash = fnv1a_mix(hash, cancel.idempotency_hash);
    return hash;
}

}  // namespace trading_engine::strategy::market_making
