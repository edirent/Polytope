#include "engine/strategy/market_making/research/MarkoutAttributionEngine.h"

namespace trading_engine::strategy::market_making::research {

FillAttribution MarkoutAttributionEngine::attribute(
    const FillAttributionInput& input
) const noexcept {
    FillAttribution out;
    out.fill_price_tick = input.fill_price_tick;
    out.fair_at_fill_tick = input.fair_at_fill_tick;
    out.mid_at_fill_tick = input.mid_at_fill_tick;
    if (input.buy) {
        out.markout_tick = input.future_mid_tick - input.fill_price_tick;
        out.fair_markout_tick = input.future_fair_tick - input.fill_price_tick;
        out.spread_capture_tick = input.mid_at_fill_tick - input.fill_price_tick;
        out.adverse_selection_tick =
            input.future_mid_tick - input.mid_at_fill_tick;
    } else {
        out.markout_tick = input.fill_price_tick - input.future_mid_tick;
        out.fair_markout_tick = input.fill_price_tick - input.future_fair_tick;
        out.spread_capture_tick = input.fill_price_tick - input.mid_at_fill_tick;
        out.adverse_selection_tick =
            input.mid_at_fill_tick - input.future_mid_tick;
    }
    out.inventory_pnl_tick = out.fair_markout_tick;
    out.reward_pnl_tick = 0;
    return out;
}

}  // namespace trading_engine::strategy::market_making::research
