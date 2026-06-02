#include "engine/strategy/market_making/core/MarketMakingEngine.h"

#include "engine/strategy/market_making/refresh/CancelReplacePlanner.h"
#include "engine/strategy/market_making/state/ActiveQuoteState.h"

namespace trading_engine::strategy::market_making {

MarketMakingEngine::MarketMakingEngine(MarketMakingConfig config)
    : config_(config) {}

const QuoteBook& MarketMakingEngine::quote_book() const noexcept {
    return quote_book_;
}

MarketMakingResult MarketMakingEngine::on_market_update(
    const MarketMakingInput& input
) {
    MarketMakingResult result;
    result.snapshots_seen = 1;

    if (!input.depth) {
        result.ok = false;
        result.rejected_no_quote = 1;
        result.output_hash = compute_market_making_result_hash(result);
        return result;
    }

    const auto active = quote_book_.find(input.asset_index);
    const auto fair =
        fair_value_model_.compute(*input.depth, config_, input.now_ns);

    QuoteBuildResult build;
    if (fair.ok) {
        const auto spread = spread_model_.compute(config_);
        const auto skew =
            inventory_skew_model_.compute(config_, input.current_position_lots);
        const auto size =
            quote_size_model_.compute(config_, *input.depth, input.current_position_lots);
        build = quote_engine_.build(QuoteBuildInput{
            .market_id = input.market_id,
            .asset_id = input.asset_id,
            .market_index = input.market_index,
            .asset_index = input.asset_index,
            .depth = input.depth,
            .config = &config_,
            .fair_value = fair,
            .spread = spread,
            .size = size,
            .inventory_skew_tick = skew,
            .current_position_lots = input.current_position_lots,
            .now_ns = input.now_ns,
            .intent_type = active ? QuoteIntentType::ReplaceQuote
                                  : QuoteIntentType::PlaceQuote
        });
    }

    const auto refresh = refresh_policy_.evaluate(
        active,
        build.ok ? &build.quote : nullptr,
        *input.depth,
        config_,
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
    }

    result.output_hash = compute_market_making_result_hash(result);
    return result;
}

}  // namespace trading_engine::strategy::market_making
