#include "engine/strategy/market_making/fair/FairValueModel.h"

namespace trading_engine::strategy::market_making {

FairValueResult FairValueModel::compute(
    const state::MarketDepthView& depth,
    const MarketMakingConfig& config,
    std::uint64_t now_ns
) const noexcept {
    FairValueResult result;
    result.source = FairValueSourceKind::Mid;

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
    if (config.max_book_age_ns > 0 && depth.last_ws_recv_ns > 0 &&
        now_ns > depth.last_ws_recv_ns &&
        static_cast<std::int64_t>(now_ns - depth.last_ws_recv_ns) >
            config.max_book_age_ns) {
        result.quality = FairValueQuality::StaleBook;
        return result;
    }

    result.ok = true;
    result.quality = FairValueQuality::Good;
    result.fair_value_tick =
        (depth.bids[0].price_tick + depth.asks[0].price_tick) / 2;
    return result;
}

}  // namespace trading_engine::strategy::market_making
