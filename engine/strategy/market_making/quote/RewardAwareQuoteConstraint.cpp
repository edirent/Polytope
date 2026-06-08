#include "engine/strategy/market_making/quote/RewardAwareQuoteConstraint.h"

#include "engine/strategy/market_making/quote/QuotePriceClamp.h"

#include <algorithm>

namespace trading_engine::strategy::market_making {
namespace {

[[nodiscard]] bool token_matches(
    const reward::RewardMarketConfig& market,
    const std::string& asset_id
) noexcept {
    for (const auto& token : market.tokens) {
        if (token.token_id == asset_id && token.eligible) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] const reward::RewardMarketConfig* find_reward_market(
    const reward::RewardConfigSnapshot& snapshot,
    const QuoteIntent& quote
) noexcept {
    for (const auto& market : snapshot.markets) {
        if (!quote.asset_id.empty() && token_matches(market, quote.asset_id)) {
            return &market;
        }
        if (!quote.market_id.empty() && market.condition_id == quote.market_id) {
            return &market;
        }
    }
    return nullptr;
}

[[nodiscard]] bool is_unwind(const QuoteIntent& quote) noexcept {
    return quote.type == QuoteIntentType::PassiveUnwind ||
           quote.type == QuoteIntentType::ForcedUnwind;
}

void raise_opening_size(
    QuoteIntent* quote,
    std::int64_t min_size_lots
) noexcept {
    if (min_size_lots <= 0) {
        return;
    }
    if (quote->has_bid && !quote->bid.risk_reducing) {
        quote->bid.quantity_lots =
            std::max(quote->bid.quantity_lots, min_size_lots);
    }
    if (quote->has_ask && !quote->ask.risk_reducing) {
        quote->ask.quantity_lots =
            std::max(quote->ask.quantity_lots, min_size_lots);
    }
}

[[nodiscard]] bool clamp_two_sided_spread(
    QuoteIntent* quote,
    const MarketMakingConfig& config,
    std::int64_t max_spread_tick
) noexcept {
    if (!quote->has_bid || !quote->has_ask || max_spread_tick <= 0) {
        return true;
    }
    const auto spread = quote->ask.price_tick - quote->bid.price_tick;
    if (spread <= max_spread_tick) {
        return true;
    }
    const auto half = std::max<std::int64_t>(1, max_spread_tick / 2);
    auto bid = clamp_tick(
        quote->fair_value_tick - half,
        config.min_price_tick,
        config.max_price_tick
    );
    auto ask = clamp_tick(
        bid + max_spread_tick,
        config.min_price_tick,
        config.max_price_tick
    );
    if (ask - bid > max_spread_tick) {
        ask = clamp_tick(
            quote->fair_value_tick + half,
            config.min_price_tick,
            config.max_price_tick
        );
        bid = clamp_tick(
            ask - max_spread_tick,
            config.min_price_tick,
            config.max_price_tick
        );
    }
    if (bid >= ask) {
        return false;
    }
    quote->bid.price_tick = bid;
    quote->ask.price_tick = ask;
    quote->bid.edge_to_fair_tick = quote->fair_value_tick - bid;
    quote->ask.edge_to_fair_tick = ask - quote->fair_value_tick;
    quote->half_spread_tick = (ask - bid) / 2;
    return true;
}

[[nodiscard]] bool active_opening_sides_eligible(
    const QuoteIntent& quote,
    std::int64_t min_size_lots,
    std::int64_t max_spread_tick
) noexcept {
    if (quote.has_bid && !quote.bid.risk_reducing &&
        quote.bid.quantity_lots < min_size_lots) {
        return false;
    }
    if (quote.has_ask && !quote.ask.risk_reducing &&
        quote.ask.quantity_lots < min_size_lots) {
        return false;
    }
    if (quote.has_bid && quote.has_ask && max_spread_tick > 0 &&
        quote.ask.price_tick - quote.bid.price_tick > max_spread_tick) {
        return false;
    }
    return true;
}

}  // namespace

bool RewardAwareQuoteConstraint::apply(
    QuoteIntent* quote,
    const MarketMakingConfig& config,
    const reward::RewardConfigSnapshot* reward_config
) const noexcept {
    if (!quote || !config.reward_aware_quotes_enabled) {
        return true;
    }
    if (is_unwind(*quote) && config.reward_allow_unwind_exemption) {
        quote->reward_reason = "reward_exempt_unwind";
        return true;
    }
    if (!reward_config) {
        quote->reward_reason = "reward_config_missing";
        return true;
    }

    const auto* market = find_reward_market(*reward_config, *quote);
    if (!market) {
        quote->reward_reason = "reward_market_missing";
        return true;
    }

    quote->reward_config_present = true;
    quote->reward_condition_id = market->condition_id;
    quote->reward_max_spread_tick = market->rewards_max_spread_tick;
    quote->reward_min_size_lots = std::max(
        market->rewards_min_size_lots,
        config.reward_min_size_lots_floor
    );

    const auto max_spread_tick = market->rewards_max_spread_tick > 0
        ? std::max<std::int64_t>(
              1,
              market->rewards_max_spread_tick -
                  config.reward_max_spread_tick_buffer
          )
        : 0;

    raise_opening_size(quote, quote->reward_min_size_lots);
    if (!clamp_two_sided_spread(quote, config, max_spread_tick)) {
        quote->reward_eligible = false;
        quote->reward_reason = "reward_spread_constraint_failed";
        return false;
    }

    quote->reward_eligible = active_opening_sides_eligible(
        *quote,
        quote->reward_min_size_lots,
        max_spread_tick
    );
    quote->reward_reason = quote->reward_eligible
        ? "reward_eligible"
        : "reward_not_eligible";
    return true;
}

}  // namespace trading_engine::strategy::market_making
