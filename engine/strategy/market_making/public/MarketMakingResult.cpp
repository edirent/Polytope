#include "engine/strategy/market_making/public/MarketMakingResult.h"

namespace trading_engine::strategy::market_making {

std::uint64_t compute_market_making_result_hash(
    const MarketMakingResult& result
) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    hash = fnv1a_mix(hash, result.ok ? 1ULL : 0ULL);
    hash = fnv1a_mix(hash, result.snapshots_seen);
    hash = fnv1a_mix(hash, result.quotes_emitted);
    hash = fnv1a_mix(hash, result.cancels_emitted);
    hash = fnv1a_mix(hash, result.replacements);
    hash = fnv1a_mix(hash, result.rejected_no_quote);
    hash = fnv1a_mix(hash, result.active_quotes);
    hash = fnv1a_mix(hash, result.approved_quotes);
    hash = fnv1a_mix(hash, result.rejected_quotes);
    hash = fnv1a_mix(hash, result.maker_fills);
    hash =
        fnv1a_mix(hash, static_cast<std::uint64_t>(result.avg_half_spread_tick));
    hash = fnv1a_mix(
        hash,
        static_cast<std::uint64_t>(result.avg_inventory_skew_tick)
    );
    hash = fnv1a_mix(hash, result.quote_uptime_ns);
    hash = fnv1a_mix(
        hash,
        static_cast<std::uint64_t>(result.maker_realized_pnl_tick)
    );
    hash = fnv1a_mix(
        hash,
        static_cast<std::uint64_t>(result.maker_unrealized_pnl_tick)
    );
    hash =
        fnv1a_mix(hash, static_cast<std::uint64_t>(result.spread_capture_tick));
    hash = fnv1a_mix(
        hash,
        static_cast<std::uint64_t>(result.adverse_selection_5s_tick)
    );
    hash = fnv1a_mix(hash, result.quote_count);
    for (std::uint16_t i = 0; i < result.quote_count; ++i) {
        hash = fnv1a_mix(hash, compute_quote_intent_hash(result.quotes[i]));
    }
    hash = fnv1a_mix(hash, result.cancel_count);
    for (std::uint16_t i = 0; i < result.cancel_count; ++i) {
        hash =
            fnv1a_mix(hash, compute_cancel_quote_intent_hash(result.cancels[i]));
    }
    return hash;
}

}  // namespace trading_engine::strategy::market_making
