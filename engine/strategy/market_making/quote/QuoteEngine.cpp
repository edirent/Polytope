#include "engine/strategy/market_making/quote/QuoteEngine.h"

#include "engine/strategy/market_making/quote/QuotePriceClamp.h"
#include "engine/strategy/market_making/quote/RewardAwareQuoteConstraint.h"

#include <algorithm>

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
    std::int64_t qty_lots,
    bool risk_reducing
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
    leg.risk_reducing = risk_reducing;
    leg.allow_fair_deviation_exemption = risk_reducing;
    leg.allow_spread_exemption = risk_reducing;
    leg.book_version = input.depth->book_version;
    leg.snapshot_version_hash = input.depth->snapshot_version_hash;
    return leg;
}

[[nodiscard]] std::int64_t best_bid_tick(
    const QuoteBuildInput& input
) noexcept {
    return input.depth && input.depth->bid_count > 0
        ? input.depth->bids[0].price_tick
        : 0;
}

[[nodiscard]] std::int64_t best_ask_tick(
    const QuoteBuildInput& input
) noexcept {
    return input.depth && input.depth->ask_count > 0
        ? input.depth->asks[0].price_tick
        : 0;
}

[[nodiscard]] std::int64_t midpoint_tick(
    const QuoteBuildInput& input
) noexcept {
    const auto bid = best_bid_tick(input);
    const auto ask = best_ask_tick(input);
    return bid > 0 && ask > 0 && bid <= ask ? (bid + ask) / 2 : 0;
}

[[nodiscard]] bool bid_reduces_inventory(
    const QuoteBuildInput& input
) noexcept {
    return input.current_position_lots < input.config->target_position_lots;
}

[[nodiscard]] bool ask_reduces_inventory(
    const QuoteBuildInput& input
) noexcept {
    return input.current_position_lots > input.config->target_position_lots;
}

[[nodiscard]] std::int64_t inventory_error_bps(
    const QuoteBuildInput& input
) noexcept {
    const auto error =
        input.current_position_lots - input.config->target_position_lots;
    const auto denominator = error >= 0
        ? std::max<std::int64_t>(
              1,
              input.config->max_inventory_lots -
                  input.config->target_position_lots
          )
        : std::max<std::int64_t>(
              1,
              input.config->target_position_lots -
                  input.config->min_inventory_lots
          );
    return static_cast<std::int64_t>(
        std::min<__int128>(
            10'000,
            static_cast<__int128>(std::llabs(error)) * 10'000 /
                denominator
        )
    );
}

[[nodiscard]] std::int64_t inventory_excess_lots(
    const QuoteBuildInput& input
) noexcept {
    return std::llabs(
        input.current_position_lots - input.config->target_position_lots
    );
}

[[nodiscard]] std::int64_t reduce_qty_cap(
    const QuoteBuildInput& input,
    QuoteSide side,
    bool forced_unwind
) noexcept {
    if (side == QuoteSide::Ask) {
        if (input.current_position_lots <= input.config->target_position_lots) {
            return 0;
        }
        const auto floor = forced_unwind || !input.config->reduce_only_quote_to_target
            ? input.config->min_inventory_lots
            : input.config->target_position_lots;
        return std::max<std::int64_t>(0, input.current_position_lots - floor);
    }

    if (input.current_position_lots >= input.config->target_position_lots) {
        return 0;
    }
    const auto ceiling = forced_unwind || !input.config->reduce_only_quote_to_target
        ? input.config->max_inventory_lots
        : input.config->target_position_lots;
    return std::max<std::int64_t>(0, ceiling - input.current_position_lots);
}

[[nodiscard]] std::int64_t passive_join_ask_tick(
    const QuoteBuildInput& input
) noexcept {
    const auto ask = best_ask_tick(input);
    if (ask <= 0) {
        return 0;
    }
    const auto offset = std::max<std::int64_t>(
        1,
        input.config->passive_reduce_join_tick
    );
    return std::max(input.config->min_price_tick, ask - offset);
}

[[nodiscard]] std::int64_t passive_join_bid_tick(
    const QuoteBuildInput& input
) noexcept {
    const auto bid = best_bid_tick(input);
    if (bid <= 0) {
        return 0;
    }
    const auto offset = std::max<std::int64_t>(
        1,
        input.config->passive_reduce_join_tick
    );
    return std::min(input.config->max_price_tick, bid + offset);
}

[[nodiscard]] QuoteIntentRiskMode classify_risk_mode(
    const QuoteIntent& quote
) noexcept {
    const auto bid_reducing = quote.has_bid && quote.bid.risk_reducing;
    const auto ask_reducing = quote.has_ask && quote.ask.risk_reducing;
    const auto bid_opening = quote.has_bid && !quote.bid.risk_reducing;
    const auto ask_opening = quote.has_ask && !quote.ask.risk_reducing;
    if ((quote.type == QuoteIntentType::ForcedUnwind) &&
        (bid_reducing || ask_reducing) && !bid_opening && !ask_opening) {
        return QuoteIntentRiskMode::ForcedReduce;
    }
    if ((bid_reducing || ask_reducing) && !bid_opening && !ask_opening) {
        return QuoteIntentRiskMode::ReduceOnly;
    }
    if ((bid_reducing || ask_reducing) && (bid_opening || ask_opening)) {
        return QuoteIntentRiskMode::Mixed;
    }
    return QuoteIntentRiskMode::Opening;
}

void clear_bid(QuoteIntent* quote) noexcept {
    quote->has_bid = false;
    quote->bid = QuoteLeg{};
}

void clear_ask(QuoteIntent* quote) noexcept {
    quote->has_ask = false;
    quote->ask = QuoteLeg{};
}

[[nodiscard]] bool resolve_self_crossed_mixed_quote(
    QuoteIntent* quote
) noexcept {
    if (!quote->has_bid || !quote->has_ask ||
        quote->bid.price_tick < quote->ask.price_tick) {
        return true;
    }

    const auto bid_reducing = quote->bid.risk_reducing;
    const auto ask_reducing = quote->ask.risk_reducing;
    if (bid_reducing && !ask_reducing) {
        clear_ask(quote);
        quote->reason = "reduce_only_bid_self_cross_resolved";
        return true;
    }
    if (ask_reducing && !bid_reducing) {
        clear_bid(quote);
        quote->reason = "reduce_only_ask_self_cross_resolved";
        return true;
    }
    return false;
}

[[nodiscard]] std::int64_t required_edge_tick(
    const MarketMakingConfig& config
) noexcept {
    return std::max<std::int64_t>(
        0,
        config.min_quote_edge_tick +
            config.fee_buffer_tick +
            config.latency_buffer_tick +
            config.adverse_selection_buffer_tick
    );
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

    const auto reservation_tick =
        input.fair_value.fair_value_tick - input.inventory_skew_tick;
    const auto raw_bid = reservation_tick - input.spread.half_spread_tick;
    const auto raw_ask = reservation_tick + input.spread.half_spread_tick;
    const auto prices = clamp_quote_prices(raw_bid, raw_ask, *input.config);
    if (!prices.ok) {
        result.reason = "crossed or invalid quote prices";
        return result;
    }
    const auto bid_tick = bid_reduces_inventory(input)
        ? prices.bid_tick
        : std::min(prices.bid_tick, input.fair_value.fair_value_tick - 1);
    const auto ask_tick = ask_reduces_inventory(input)
        ? prices.ask_tick
        : std::max(prices.ask_tick, input.fair_value.fair_value_tick + 1);
    const auto position_error =
        input.current_position_lots - input.config->target_position_lots;
    const auto excess_lots = inventory_excess_lots(input);
    const auto error_bps = inventory_error_bps(input);
    const auto forced_unwind =
        input.config->forced_unwind_position_bps > 0 &&
        error_bps >= input.config->forced_unwind_position_bps;
    const auto urgent_unwind =
        !forced_unwind &&
        input.config->urgent_reduce_excess_lots > 0 &&
        excess_lots >= input.config->urgent_reduce_excess_lots;
    const auto passive_unwind =
        !forced_unwind && !urgent_unwind &&
        ((input.config->passive_unwind_position_bps > 0 &&
          error_bps >= input.config->passive_unwind_position_bps) ||
         (input.config->passive_reduce_excess_lots > 0 &&
          excess_lots >= input.config->passive_reduce_excess_lots));
    const auto bid_reduces = bid_reduces_inventory(input);
    const auto ask_reduces = ask_reduces_inventory(input);
    const auto edge_required = required_edge_tick(*input.config);
    const auto external_fair_opening_ok =
        !input.config->require_external_fair_for_opening_quotes ||
        input.fair_value.external_fair_value_tick > 0;

    QuoteIntent quote;
    quote.type = forced_unwind
        ? QuoteIntentType::ForcedUnwind
        : ((urgent_unwind || passive_unwind) ? QuoteIntentType::PassiveUnwind
                          : input.intent_type);
    quote.strategy_id = input.config->strategy_id;
    quote.quote_group_id =
        quote_group_id(input.config->strategy_id, input.asset_index);
    quote.market_id = input.market_id;
    quote.asset_id = input.asset_id;
    quote.market_index = input.market_index;
    quote.asset_index = input.asset_index;
    quote.fair_value_tick = input.fair_value.fair_value_tick;
    quote.asset_fair_tick = input.fair_value.fair_value_tick;
    quote.canonical_yes_fair_tick = input.canonical_yes_fair_tick;
    quote.canonical_yes_position_lots = input.canonical_yes_position_lots;
    quote.target_canonical_yes_lots = input.target_canonical_yes_lots;
    quote.edge_tick = input.canonical_yes_fair_tick > 0
        ? input.canonical_yes_fair_tick - input.fair_value.fair_value_tick
        : 0;
    quote.half_spread_tick = input.spread.half_spread_tick;
    quote.inventory_skew_tick = input.inventory_skew_tick;
    quote.target_position_lots = input.config->target_position_lots;
    quote.current_position_lots = input.current_position_lots;
    quote.created_ts_ns = input.now_ns;
    quote.expires_at_ns = input.now_ns + input.config->quote_ttl_ns;
    quote.snapshot_version_hash = input.depth->snapshot_version_hash;
    quote.oracle_artifact_hash = input.config->oracle_artifact_hash;
    quote.policy_hash = input.config->policy_hash;
    quote.reason = forced_unwind
        ? "forced_unwind_quote"
        : (urgent_unwind ? "urgent_unwind_quote"
                         : (passive_unwind ? "passive_unwind_quote"
                                           : "reservation_fair_quote"));

    if (input.config->enable_bid_quotes &&
        input.size.bid_qty_lots > 0 &&
        bid_tick >= input.config->min_price_tick) {
        const auto opening_edge =
            input.fair_value.fair_value_tick - bid_tick;
        const auto allow_bid =
            (!forced_unwind && !urgent_unwind && !passive_unwind &&
             (external_fair_opening_ok || bid_reduces) &&
             opening_edge >= edge_required) ||
            (position_error < 0 &&
             (passive_unwind || urgent_unwind || forced_unwind));
        if (allow_bid) {
            auto final_bid_tick = bid_tick;
            auto final_bid_qty = input.size.bid_qty_lots;
            if (forced_unwind && bid_reduces &&
                input.depth->ask_count > 0 &&
                input.depth->asks[0].price_tick > 0) {
                final_bid_tick = input.depth->asks[0].price_tick;
            } else if (urgent_unwind && bid_reduces) {
                const auto mid = midpoint_tick(input);
                if (mid > 0) {
                    final_bid_tick = std::max(final_bid_tick, mid);
                }
                if (input.config->urgent_unwind_aggression_tick > 0) {
                    final_bid_tick = std::min(
                        input.config->max_price_tick,
                        final_bid_tick +
                            input.config->urgent_unwind_aggression_tick
                    );
                }
            } else if ((passive_unwind || bid_reduces) && bid_reduces) {
                const auto joined = passive_join_bid_tick(input);
                if (joined > 0) {
                    final_bid_tick = std::max(final_bid_tick, joined);
                }
            }
            if (passive_unwind && bid_reduces &&
                input.config->passive_unwind_aggression_tick > 0) {
                final_bid_tick = std::min(
                    input.config->max_price_tick,
                    final_bid_tick +
                        input.config->passive_unwind_aggression_tick
                );
            }
            if (bid_reduces) {
                final_bid_qty = std::min(
                    final_bid_qty,
                    reduce_qty_cap(input, QuoteSide::Bid, forced_unwind)
                );
            }
            if (final_bid_qty <= 0) {
                final_bid_tick = 0;
            }
            if (final_bid_tick > 0) {
                final_bid_tick = clamp_tick(
                    final_bid_tick,
                    input.config->min_price_tick,
                    input.config->max_price_tick
                );
                quote.has_bid = true;
                quote.bid =
                    make_leg(
                        input,
                        QuoteSide::Bid,
                        final_bid_tick,
                        final_bid_qty,
                        bid_reduces
                    );
            }
        }
    }
    if (input.config->enable_ask_quotes &&
        input.size.ask_qty_lots > 0 &&
        ask_tick <= input.config->max_price_tick) {
        const auto opening_edge =
            ask_tick - input.fair_value.fair_value_tick;
        const auto allow_ask =
            (!forced_unwind && !urgent_unwind && !passive_unwind &&
             (external_fair_opening_ok || ask_reduces) &&
             opening_edge >= edge_required) ||
            (position_error > 0 &&
             (passive_unwind || urgent_unwind || forced_unwind));
        if (allow_ask) {
            auto final_ask_tick = ask_tick;
            auto final_ask_qty = input.size.ask_qty_lots;
            if (forced_unwind && ask_reduces &&
                input.depth->bid_count > 0 &&
                input.depth->bids[0].price_tick > 0) {
                final_ask_tick = input.depth->bids[0].price_tick;
            } else if (urgent_unwind && ask_reduces) {
                const auto mid = midpoint_tick(input);
                if (mid > 0) {
                    final_ask_tick = std::min(final_ask_tick, mid);
                }
                if (input.config->urgent_unwind_aggression_tick > 0) {
                    final_ask_tick = std::max(
                        input.config->min_price_tick,
                        final_ask_tick -
                            input.config->urgent_unwind_aggression_tick
                    );
                }
            } else if ((passive_unwind || ask_reduces) && ask_reduces) {
                const auto joined = passive_join_ask_tick(input);
                if (joined > 0) {
                    final_ask_tick = std::min(final_ask_tick, joined);
                }
            }
            if (passive_unwind && ask_reduces &&
                input.config->passive_unwind_aggression_tick > 0) {
                final_ask_tick = std::max(
                    input.config->min_price_tick,
                    final_ask_tick -
                        input.config->passive_unwind_aggression_tick
                );
            }
            if (ask_reduces) {
                final_ask_qty = std::min(
                    final_ask_qty,
                    reduce_qty_cap(input, QuoteSide::Ask, forced_unwind)
                );
            }
            if (final_ask_qty <= 0) {
                final_ask_tick = 0;
            }
            if (final_ask_tick > 0) {
                final_ask_tick = clamp_tick(
                    final_ask_tick,
                    input.config->min_price_tick,
                    input.config->max_price_tick
                );
                quote.has_ask = true;
                quote.ask =
                    make_leg(
                        input,
                        QuoteSide::Ask,
                        final_ask_tick,
                        final_ask_qty,
                        ask_reduces
                    );
            }
        }
    }
    if (!resolve_self_crossed_mixed_quote(&quote)) {
        result.reason = "crossed or invalid quote prices";
        return result;
    }
    if (!quote.has_bid && !quote.has_ask) {
        result.reason = "no quote sides";
        return result;
    }
    RewardAwareQuoteConstraint reward_constraint;
    if (!reward_constraint.apply(&quote, *input.config, input.reward_config)) {
        result.reason = "reward quote constraint failed";
        return result;
    }
    if (!resolve_self_crossed_mixed_quote(&quote)) {
        result.reason = "crossed or invalid quote prices";
        return result;
    }

    quote.risk_mode = classify_risk_mode(quote);
    quote.idempotency_hash = compute_quote_intent_hash(quote);
    quote.quote_intent_id = quote.idempotency_hash;
    result.ok = true;
    result.quote = quote;
    return result;
}

}  // namespace trading_engine::strategy::market_making
