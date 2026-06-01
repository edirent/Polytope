#include "engine/paper/pnl/MarkPriceProvider.h"

namespace trading_engine::paper {

namespace {

[[nodiscard]] bool degraded(
    const trading_engine::state::MarketDepthView& view
) noexcept {
    return !view.usable_for_depth ||
           view.recovering ||
           view.crossed ||
           view.closed ||
           view.resolved;
}

}  // namespace

MarkPrice MarkPriceProvider::mark_from_depth(
    const trading_engine::state::MarketDepthView& view
) const noexcept {
    MarkPrice mark;
    mark.asset_index = view.asset_index;

    if (degraded(view)) {
        mark.mid_quality = MarkQuality::Degraded;
        mark.liquidation_quality = MarkQuality::Degraded;
    }

    if (view.bid_count == 0) {
        mark.liquidation_mark_tick = 0;
        mark.liquidation_quality = MarkQuality::MissingBid;
    } else {
        mark.liquidation_mark_tick = view.bids[0].price_tick;
        if (!degraded(view)) {
            mark.liquidation_quality = MarkQuality::Good;
        }
    }

    if (view.bid_count == 0 && view.ask_count == 0) {
        mark.mid_mark_tick = 0;
        mark.mid_quality = MarkQuality::MissingBook;
        return mark;
    }
    if (view.bid_count == 0) {
        mark.mid_mark_tick = 0;
        mark.mid_quality = MarkQuality::MissingBid;
        return mark;
    }
    if (view.ask_count == 0) {
        mark.mid_mark_tick = 0;
        mark.mid_quality = MarkQuality::MissingAsk;
        return mark;
    }

    mark.mid_mark_tick =
        (view.bids[0].price_tick + view.asks[0].price_tick) / 2;
    if (!degraded(view)) {
        mark.mid_quality = MarkQuality::Good;
    }
    return mark;
}

}  // namespace trading_engine::paper
