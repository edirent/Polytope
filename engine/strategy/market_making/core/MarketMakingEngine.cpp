#include "engine/strategy/market_making/core/MarketMakingEngine.h"

#include "engine/strategy/market_making/refresh/CancelReplacePlanner.h"
#include "engine/strategy/market_making/state/ActiveQuoteState.h"

#include <algorithm>

namespace trading_engine::strategy::market_making {

namespace {

[[nodiscard]] NoQuoteReason reason_from_quote_build(
    const QuoteBuildResult& build
) noexcept {
    if (build.reason == "fair value unavailable") {
        return NoQuoteReason::FairValueUnavailable;
    }
    if (build.reason == "base quote size <= 0" ||
        build.reason == "no inventory room or depth") {
        return NoQuoteReason::QuoteSizeUnavailable;
    }
    if (build.reason == "crossed or invalid quote prices") {
        return NoQuoteReason::InvalidQuotePrices;
    }
    if (build.reason == "no quote sides") {
        return NoQuoteReason::NoQuoteSides;
    }
    return NoQuoteReason::QuoteBuildFailed;
}

[[nodiscard]] NoQuoteReason reason_from_fair_quality(
    FairValueQuality quality
) noexcept {
    switch (quality) {
        case FairValueQuality::StaleBook:
            return NoQuoteReason::FairStaleBook;
        case FairValueQuality::CrossedBook:
            return NoQuoteReason::FairCrossedBook;
        case FairValueQuality::MissingBidAsk:
            return NoQuoteReason::FairMissingLiquidity;
        case FairValueQuality::LowConfidence:
            return NoQuoteReason::FairLowConfidence;
        case FairValueQuality::ExternalStale:
            return NoQuoteReason::FairExternalStale;
        case FairValueQuality::Valid:
        case FairValueQuality::Disabled:
            return NoQuoteReason::FairUnavailableUnknown;
    }
    return NoQuoteReason::FairUnavailableUnknown;
}

}  // namespace

MarketMakingEngine::MarketMakingEngine(MarketMakingConfig config)
    : config_(config) {}

const QuoteBook& MarketMakingEngine::quote_book() const noexcept {
    return quote_book_;
}

bool MarketMakingEngine::remove_active_quote(std::uint32_t asset_index) {
    return quote_book_.remove(asset_index);
}

MarketMakingResult MarketMakingEngine::on_market_update(
    const MarketMakingInput& input
) {
    MarketMakingResult result;
    result.snapshots_seen = 1;

    if (!input.depth) {
        result.ok = false;
        result.rejected_no_quote = 1;
        result.no_quote_reason = NoQuoteReason::MissingDepth;
        result.output_hash = compute_market_making_result_hash(result);
        return result;
    }

    auto effective_config = config_;
    if (input.external_fair_value_tick > 0) {
        effective_config.external_fair_value_tick =
            input.external_fair_value_tick;
    }
    if (input.dynamic_target_position_lots != 0 ||
        input.dynamic_min_inventory_lots != 0 ||
        input.dynamic_max_inventory_lots != 0) {
        effective_config.target_position_lots =
            input.dynamic_target_position_lots;
        effective_config.min_inventory_lots =
            input.dynamic_min_inventory_lots;
        effective_config.max_inventory_lots =
            input.dynamic_max_inventory_lots;
    }
    if (input.dynamic_half_spread_tick > 0) {
        effective_config.min_half_spread_tick =
            input.dynamic_half_spread_tick;
    } else if (input.dynamic_min_half_spread_tick > 0) {
        effective_config.min_half_spread_tick = std::max(
            effective_config.min_half_spread_tick,
            input.dynamic_min_half_spread_tick
        );
    }
    if (input.dynamic_max_inventory_skew_tick > 0) {
        effective_config.max_inventory_skew_tick =
            input.dynamic_max_inventory_skew_tick;
    }
    if (input.disable_bid_quotes) {
        effective_config.enable_bid_quotes = false;
    }
    if (input.disable_ask_quotes) {
        effective_config.enable_ask_quotes = false;
    }

    const auto active = quote_book_.find(input.asset_index);
    const auto fair = fair_value_model_.compute(
        *input.depth,
        effective_config,
        input.now_ns,
        input.complement_depth
    );
    result.fair_value_quality = fair.quality;
    result.fair_confidence_bps = fair.confidence_bps;
    result.fair_book_spread_tick = fair.book_spread_tick;
    result.fair_value_tick = fair.fair_value_tick;

    QuoteBuildResult build;
    if (fair.ok) {
        const auto spread = spread_model_.compute(effective_config);
        const auto skew =
            inventory_skew_model_.compute(
                effective_config,
                input.current_position_lots,
                input.time_to_expiry_ns
            );
        const auto size =
            quote_size_model_.compute(
                effective_config,
                *input.depth,
                input.current_position_lots
            );
        build = quote_engine_.build(QuoteBuildInput{
            .market_id = input.market_id,
            .asset_id = input.asset_id,
            .market_index = input.market_index,
            .asset_index = input.asset_index,
            .depth = input.depth,
            .config = &effective_config,
            .fair_value = fair,
            .spread = spread,
            .size = size,
            .reward_config = input.reward_config,
            .inventory_skew_tick = skew,
            .canonical_yes_fair_tick = input.canonical_yes_fair_value_tick,
            .canonical_yes_position_lots = input.canonical_yes_position_lots,
            .target_canonical_yes_lots = input.target_canonical_yes_lots,
            .current_position_lots = input.current_position_lots,
            .now_ns = input.now_ns,
            .intent_type = active ? QuoteIntentType::ReplaceQuote
                                  : QuoteIntentType::PlaceQuote
        });
    } else {
        build.reason = fair_value_quality_name(fair.quality);
    }

    const auto refresh = refresh_policy_.evaluate(
        active,
        build.ok ? &build.quote : nullptr,
        *input.depth,
        effective_config,
        input.current_position_lots,
        input.now_ns
    );

    if (refresh.should_cancel && active) {
        result.cancels[result.cancel_count++] =
            make_cancel_intent(*active, refresh.reason, input.now_ns);
        result.cancels_emitted = result.cancel_count;
        quote_book_.remove(input.asset_index);
    }

    if (build.ok && (!active || refresh.should_replace)) {
        if (active && refresh.should_replace) {
            result.replacements = 1;
        }
        result.quotes[result.quote_count++] = build.quote;
        result.quotes_emitted = result.quote_count;
        quote_book_.upsert(active_quote_from_intent(build.quote));
    } else if (!build.ok) {
        result.rejected_no_quote = 1;
        result.no_quote_reason = fair.ok
            ? reason_from_quote_build(build)
            : reason_from_fair_quality(fair.quality);
    }

    result.output_hash = compute_market_making_result_hash(result);
    return result;
}

}  // namespace trading_engine::strategy::market_making
