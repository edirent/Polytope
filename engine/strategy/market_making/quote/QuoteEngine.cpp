#include "engine/strategy/market_making/quote/QuoteEngine.h"

#include "engine/strategy/market_making/quote/QuotePriceClamp.h"

namespace trading_engine::strategy::market_making {

namespace {

[[nodiscard]] std::uint64_t quote_group_id(
    std::uint64_t strategy_id,
    std::uint32_t asset_index
) noexcept {
    auto hash = 14695981039346656037ULL;
    hash = fnv1a_mix(hash, strategy_id);
    hash = fnv1a_mix(hash, asset_index);
    return hash;
}

[[nodiscard]] QuoteLeg make_leg(
    const QuoteBuildInput& input,
    QuoteSide side,
    std::int64_t price_tick,
    std::int64_t qty_lots
) {
    QuoteLeg leg;
    leg.market_id = input.market_id;
    leg.asset_id = input.asset_id;
    leg.market_index = input.market_index;
    leg.asset_index = input.asset_index;
    leg.side = side;
    leg.price_tick = price_tick;
    leg.quantity_lots = qty_lots;
    leg.fair_value_tick = input.fair_value.fair_value_tick;
    leg.edge_to_fair_tick =
        side == QuoteSide::Bid ? input.fair_value.fair_value_tick - price_tick
                               : price_tick - input.fair_value.fair_value_tick;
    leg.book_version = input.depth->book_version;
    leg.snapshot_version_hash = input.depth->snapshot_version_hash;
    return leg;
}

}  // namespace

QuoteBuildResult QuoteEngine::build(const QuoteBuildInput& input) const {
    QuoteBuildResult result;
    if (!input.depth || !input.config) {
        result.reason = "missing depth or config";
        return result;
    }
    if (!input.fair_value.ok) {
        result.reason = "fair value unavailable";
        return result;
    }
    if (!input.size.ok) {
        result.reason = input.size.reason;
        return result;
    }

    const auto raw_bid = input.fair_value.fair_value_tick -
                         input.spread.half_spread_tick -
                         input.inventory_skew_tick;
    const auto raw_ask = input.fair_value.fair_value_tick +
                         input.spread.half_spread_tick -
                         input.inventory_skew_tick;
    const auto prices = clamp_quote_prices(raw_bid, raw_ask, *input.config);
    if (!prices.ok) {
        result.reason = "crossed or invalid quote prices";
        return result;
    }

    QuoteIntent quote;
    quote.type = input.intent_type;
    quote.strategy_id = input.config->strategy_id;
    quote.quote_group_id =
        quote_group_id(input.config->strategy_id, input.asset_index);
    quote.market_id = input.market_id;
    quote.asset_id = input.asset_id;
    quote.market_index = input.market_index;
    quote.asset_index = input.asset_index;
    quote.fair_value_tick = input.fair_value.fair_value_tick;
    quote.half_spread_tick = input.spread.half_spread_tick;
    quote.inventory_skew_tick = input.inventory_skew_tick;
    quote.target_position_lots = input.config->target_position_lots;
    quote.current_position_lots = input.current_position_lots;
    quote.created_ts_ns = input.now_ns;
    quote.expires_at_ns = input.now_ns + input.config->quote_ttl_ns;
    quote.snapshot_version_hash = input.depth->snapshot_version_hash;
    quote.oracle_artifact_hash = input.config->oracle_artifact_hash;
    quote.policy_hash = input.config->policy_hash;
    quote.reason = "fair_mid_quote";

    if (input.size.bid_qty_lots > 0) {
        quote.has_bid = true;
        quote.bid =
            make_leg(input, QuoteSide::Bid, prices.bid_tick, input.size.bid_qty_lots);
    }
    if (input.size.ask_qty_lots > 0) {
        quote.has_ask = true;
        quote.ask =
            make_leg(input, QuoteSide::Ask, prices.ask_tick, input.size.ask_qty_lots);
    }
    if (!quote.has_bid && !quote.has_ask) {
        result.reason = "no quote sides";
        return result;
    }

    quote.idempotency_hash = compute_quote_intent_hash(quote);
    quote.quote_intent_id = quote.idempotency_hash;
    result.ok = true;
    result.quote = quote;
    return result;
}

}  // namespace trading_engine::strategy::market_making
