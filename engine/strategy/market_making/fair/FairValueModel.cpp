#include "engine/strategy/market_making/fair/FairValueModel.h"

#include "engine/state/book/DepthPrefix.h"

#include <algorithm>
#include <cmath>

namespace trading_engine::strategy::market_making {

namespace {

[[nodiscard]] std::int64_t bps_to_tick(
    std::int64_t fair_tick,
    std::int64_t bps
) noexcept {
    if (fair_tick <= 0 || bps <= 0) {
        return 0;
    }
    return std::max<std::int64_t>(
        1,
        static_cast<std::int64_t>(
            static_cast<__int128>(fair_tick) * bps / 10'000
        )
    );
}

[[nodiscard]] std::int64_t clamp_round(long double value) noexcept {
    return static_cast<std::int64_t>(std::llround(value));
}

[[nodiscard]] state::PrefixVwapResult sell_vwap_from_prefix(
    const state::MarketDepthView& depth,
    std::int64_t qty_lots
) noexcept {
    state::PrefixVwapResult result;
    result.qty_lots = qty_lots;

    if (qty_lots <= 0 || depth.prefix.bid_count == 0 ||
        depth.prefix.bid_cum_qty[depth.prefix.bid_count - 1U] < qty_lots) {
        return result;
    }

    std::uint16_t level_index = 0;
    for (; level_index < depth.prefix.bid_count; ++level_index) {
        if (depth.prefix.bid_cum_qty[level_index] >= qty_lots) {
            break;
        }
    }
    if (level_index >= depth.prefix.bid_count) {
        return result;
    }

    const auto qty_before =
        level_index == 0 ? 0 : depth.prefix.bid_cum_qty[level_index - 1U];
    const auto proceeds_before =
        level_index == 0 ? 0
                         : depth.prefix.bid_cum_proceeds[level_index - 1U];
    const auto remaining = qty_lots - qty_before;
    const auto price_tick = depth.bids[level_index].price_tick;
    if (remaining < 0 || price_tick <= 0) {
        return result;
    }

    std::int64_t partial_proceeds = 0;
    if (!state::depth_prefix_checked_mul(
            price_tick,
            remaining,
            &partial_proceeds
        )) {
        return result;
    }

    std::int64_t total_proceeds = 0;
    if (!state::depth_prefix_checked_add(
            proceeds_before,
            partial_proceeds,
            &total_proceeds
        )) {
        return result;
    }

    result.ok = true;
    result.total_cost_tick = total_proceeds;
    result.vwap_tick = total_proceeds / qty_lots;
    result.worst_price_tick = price_tick;
    return result;
}

[[nodiscard]] std::int64_t fair_confidence_bps(
    const MarketMakingConfig& config,
    std::int64_t midpoint_tick,
    std::int64_t spread_tick,
    std::int64_t bid_qty,
    std::int64_t ask_qty,
    std::int64_t bid_depth,
    std::int64_t ask_depth
) noexcept {
    auto confidence = 10'000LL;

    if (midpoint_tick > 0) {
        const auto spread_bps = static_cast<std::int64_t>(
            static_cast<__int128>(spread_tick) * 10'000 / midpoint_tick
        );
        confidence -= std::min<std::int64_t>(4'000, spread_bps * 50);
    }

    const auto top_total = bid_qty + ask_qty;
    if (top_total > 0) {
        const auto imbalance_bps =
            static_cast<std::int64_t>(
                static_cast<__int128>(std::llabs(bid_qty - ask_qty)) *
                10'000 / top_total
            );
        confidence -= std::min<std::int64_t>(2'000, imbalance_bps / 4);
    }

    const auto min_depth = std::min(bid_depth, ask_depth);
    if (config.min_total_depth_lots > 0 &&
        min_depth < config.min_total_depth_lots) {
        confidence -= 3'000;
    }
    if (config.min_top_depth_lots > 0 &&
        std::min(bid_qty, ask_qty) < config.min_top_depth_lots) {
        confidence -= 3'000;
    }

    return std::clamp<std::int64_t>(confidence, 0, 10'000);
}

}  // namespace

namespace {

FairValueResult compute_book_fair(
    const state::MarketDepthView& depth,
    const MarketMakingConfig& config,
    std::uint64_t now_ns
) noexcept {
    FairValueResult result;
    result.source = FairValueSourceKind::MicropriceVwap;

    if (!depth.usable_for_depth || depth.recovering || depth.closed ||
        depth.resolved) {
        result.quality = FairValueQuality::StaleBook;
        return result;
    }
    if (depth.crossed) {
        result.quality = FairValueQuality::CrossedBook;
        return result;
    }
    if (depth.bid_count == 0 || depth.ask_count == 0 ||
        depth.bids[0].price_tick <= 0 || depth.asks[0].price_tick <= 0) {
        result.quality = FairValueQuality::MissingBidAsk;
        return result;
    }
    if (depth.bids[0].price_tick >= depth.asks[0].price_tick) {
        result.quality = FairValueQuality::CrossedBook;
        return result;
    }
    result.book_spread_tick =
        depth.asks[0].price_tick - depth.bids[0].price_tick;
    result.midpoint_tick =
        (depth.bids[0].price_tick + depth.asks[0].price_tick) / 2;
    if (config.max_book_spread_tick > 0 &&
        result.book_spread_tick > config.max_book_spread_tick) {
        result.quality = FairValueQuality::LowConfidence;
        return result;
    }
    const auto max_spread_by_bps =
        bps_to_tick(result.midpoint_tick, config.max_book_spread_bps);
    if (max_spread_by_bps > 0 &&
        result.book_spread_tick > max_spread_by_bps) {
        result.quality = FairValueQuality::LowConfidence;
        return result;
    }
    if (config.max_book_age_ns > 0 && depth.last_ws_recv_ns > 0 &&
        now_ns > depth.last_ws_recv_ns &&
        static_cast<std::int64_t>(now_ns - depth.last_ws_recv_ns) >
            config.max_book_age_ns) {
        result.quality = FairValueQuality::StaleBook;
        return result;
    }

    result.top_bid_qty_lots =
        state::depth_prefix_level_size_lots(depth.bids[0]);
    result.top_ask_qty_lots =
        state::depth_prefix_level_size_lots(depth.asks[0]);
    const auto bid_depth = state::bid_depth_from_prefix(depth.prefix);
    const auto ask_depth = state::ask_depth_from_prefix(depth.prefix);
    if ((config.min_top_depth_lots > 0 &&
         std::min(result.top_bid_qty_lots, result.top_ask_qty_lots) <
             config.min_top_depth_lots) ||
        (config.min_total_depth_lots > 0 &&
         std::min(bid_depth, ask_depth) < config.min_total_depth_lots)) {
        result.quality = FairValueQuality::LowConfidence;
        return result;
    }

    const auto top_qty_sum = result.top_bid_qty_lots + result.top_ask_qty_lots;
    result.microprice_tick = top_qty_sum > 0
        ? clamp_round(
              (static_cast<long double>(depth.bids[0].price_tick) *
                   result.top_ask_qty_lots +
               static_cast<long double>(depth.asks[0].price_tick) *
                   result.top_bid_qty_lots) /
              static_cast<long double>(top_qty_sum)
          )
        : result.midpoint_tick;

    const auto vwap_qty =
        std::min({
            std::max<std::int64_t>(1, config.fair_vwap_lots),
            bid_depth,
            ask_depth
        });
    const auto buy_vwap = state::buy_vwap_from_prefix(depth, vwap_qty);
    const auto sell_vwap = sell_vwap_from_prefix(depth, vwap_qty);
    result.vwap_mid_tick = buy_vwap.ok && sell_vwap.ok
        ? (buy_vwap.vwap_tick + sell_vwap.vwap_tick) / 2
        : result.midpoint_tick;

    const auto total_weight =
        config.fair_mid_weight_bps +
        config.fair_microprice_weight_bps +
        config.fair_vwap_weight_bps;
    if (total_weight <= 0) {
        result.quality = FairValueQuality::LowConfidence;
        return result;
    }
    result.fair_value_tick = clamp_round(
        (static_cast<long double>(result.midpoint_tick) *
             config.fair_mid_weight_bps +
         static_cast<long double>(result.microprice_tick) *
             config.fair_microprice_weight_bps +
         static_cast<long double>(result.vwap_mid_tick) *
             config.fair_vwap_weight_bps) /
        static_cast<long double>(total_weight)
    );
    result.confidence_bps = fair_confidence_bps(
        config,
        result.midpoint_tick,
        result.book_spread_tick,
        result.top_bid_qty_lots,
        result.top_ask_qty_lots,
        bid_depth,
        ask_depth
    );
    if (result.confidence_bps < config.min_fair_confidence_bps) {
        result.quality = FairValueQuality::LowConfidence;
        return result;
    }

    result.ok = true;
    result.quality = FairValueQuality::Valid;
    return result;
}

}  // namespace

namespace {

[[nodiscard]] std::int64_t external_fair_tick(
    const MarketMakingConfig& config
) noexcept {
    if (config.external_fair_weight_bps <= 0 ||
        config.external_fair_value_tick <= 0) {
        return 0;
    }
    return clamp_tick(
        config.external_fair_value_tick,
        config.min_price_tick,
        config.max_price_tick
    );
}

[[nodiscard]] MarketMakingConfig external_reference_book_config(
    MarketMakingConfig config
) noexcept {
    config.max_book_spread_tick = 0;
    config.max_book_spread_bps = 0;
    config.min_top_depth_lots = 0;
    config.min_total_depth_lots = 0;
    config.min_fair_confidence_bps = 0;
    if (config.fair_mid_weight_bps + config.fair_microprice_weight_bps +
            config.fair_vwap_weight_bps <=
        0) {
        config.fair_mid_weight_bps = 10'000;
        config.fair_microprice_weight_bps = 0;
        config.fair_vwap_weight_bps = 0;
    }
    return config;
}

[[nodiscard]] FairValueResult apply_external_fair(
    FairValueResult result,
    std::int64_t external_tick
) noexcept {
    result.ok = true;
    result.quality = FairValueQuality::Valid;
    result.external_fair_value_tick = external_tick;
    result.fair_value_tick = external_tick;
    result.source = FairValueSourceKind::External;
    return result;
}

}  // namespace

FairValueResult FairValueModel::compute(
    const state::MarketDepthView& depth,
    const MarketMakingConfig& config,
    std::uint64_t now_ns,
    const state::MarketDepthView* complement_depth
) const noexcept {
    const auto external_tick = external_fair_tick(config);
    if (config.external_fair_weight_bps > 0 && external_tick <= 0 &&
        config.require_external_fair_for_opening_quotes) {
        FairValueResult result;
        result.quality = FairValueQuality::ExternalStale;
        return result;
    }

    auto result = compute_book_fair(depth, config, now_ns);
    if (!result.ok && result.quality == FairValueQuality::LowConfidence &&
        external_tick > 0) {
        result = compute_book_fair(
            depth,
            external_reference_book_config(config),
            now_ns
        );
    }
    if (!result.ok) {
        return result;
    }

    if (complement_depth && config.complement_fair_weight_bps > 0) {
        const auto complement = compute_book_fair(
            *complement_depth,
            config,
            now_ns
        );
        if (complement.ok) {
            const auto complement_weight =
                std::clamp<std::int64_t>(
                    config.complement_fair_weight_bps,
                    0,
                    10'000
                );
            const auto primary_weight = 10'000 - complement_weight;
            const auto implied_tick = clamp_tick(
                kPriceOneTick - complement.fair_value_tick,
                config.min_price_tick,
                config.max_price_tick
            );
            result.complement_midpoint_tick = complement.midpoint_tick;
            result.complement_implied_tick = implied_tick;
            result.fair_value_tick = clamp_tick(
                clamp_round(
                    (static_cast<long double>(result.fair_value_tick) *
                         primary_weight +
                     static_cast<long double>(implied_tick) *
                         complement_weight) /
                    10'000.0L
                ),
                config.min_price_tick,
                config.max_price_tick
            );
            result.confidence_bps =
                std::min(result.confidence_bps, complement.confidence_bps);
            result.source = FairValueSourceKind::ComplementMarket;
        }
    }

    if (external_tick > 0) {
        result = apply_external_fair(result, external_tick);
    }
    return result;
}

}  // namespace trading_engine::strategy::market_making
