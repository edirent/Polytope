#include "engine/strategy/market_making/public/QuoteIntent.h"

namespace trading_engine::strategy::market_making {

std::uint64_t compute_quote_leg_hash(const QuoteLeg& leg) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    hash = stable_hash_string(hash, leg.market_id);
    hash = stable_hash_string(hash, leg.asset_id);
    hash = fnv1a_mix(hash, leg.market_index);
    hash = fnv1a_mix(hash, leg.asset_index);
    hash = fnv1a_mix(hash, static_cast<std::uint8_t>(leg.side));
    hash = fnv1a_mix(hash, static_cast<std::uint64_t>(leg.price_tick));
    hash = fnv1a_mix(hash, static_cast<std::uint64_t>(leg.quantity_lots));
    hash = fnv1a_mix(hash, static_cast<std::uint64_t>(leg.fair_value_tick));
    hash = fnv1a_mix(hash, static_cast<std::uint64_t>(leg.edge_to_fair_tick));
    hash = fnv1a_mix(hash, leg.book_version);
    hash = fnv1a_mix(hash, leg.snapshot_version_hash);
    return hash;
}

std::uint64_t compute_quote_intent_hash(const QuoteIntent& intent) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    hash = fnv1a_mix(hash, intent.quote_intent_id);
    hash = fnv1a_mix(hash, static_cast<std::uint8_t>(intent.type));
    hash = fnv1a_mix(hash, intent.strategy_id);
    hash = fnv1a_mix(hash, intent.quote_group_id);
    hash = stable_hash_string(hash, intent.market_id);
    hash = stable_hash_string(hash, intent.asset_id);
    hash = fnv1a_mix(hash, intent.market_index);
    hash = fnv1a_mix(hash, intent.asset_index);
    hash = fnv1a_mix(hash, intent.has_bid ? 1ULL : 0ULL);
    hash = fnv1a_mix(hash, intent.has_ask ? 1ULL : 0ULL);
    if (intent.has_bid) {
        hash = fnv1a_mix(hash, compute_quote_leg_hash(intent.bid));
    }
    if (intent.has_ask) {
        hash = fnv1a_mix(hash, compute_quote_leg_hash(intent.ask));
    }
    hash = fnv1a_mix(hash, static_cast<std::uint64_t>(intent.fair_value_tick));
    hash = fnv1a_mix(hash, static_cast<std::uint64_t>(intent.half_spread_tick));
    hash =
        fnv1a_mix(hash, static_cast<std::uint64_t>(intent.inventory_skew_tick));
    hash = fnv1a_mix(hash, intent.created_ts_ns);
    hash = fnv1a_mix(hash, intent.expires_at_ns);
    hash = fnv1a_mix(hash, intent.snapshot_version_hash);
    hash = fnv1a_mix(hash, intent.oracle_artifact_hash);
    hash = fnv1a_mix(hash, intent.policy_hash);
    hash = fnv1a_mix(hash, intent.idempotency_hash);
    return hash;
}

}  // namespace trading_engine::strategy::market_making
