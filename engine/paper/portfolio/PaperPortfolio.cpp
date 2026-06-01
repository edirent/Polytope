#include "engine/paper/portfolio/PaperPortfolio.h"

namespace trading_engine::paper {

PositionLedgerApplyResult PaperPortfolio::apply_fill(const FillApplication& fill) {
    PositionFill position_fill;
    position_fill.asset_id = fill.asset_id;
    position_fill.asset_index = fill.asset_index;
    position_fill.side = fill.side;
    position_fill.qty_lots = fill.report.filled_lots;
    position_fill.price_tick = fill.report.avg_fill_price_tick;
    return positions_.apply_fill(position_fill);
}

void PaperPortfolio::mark_mid(
    const std::string& asset_id,
    std::int64_t mid_tick
) {
    positions_.mark_mid(asset_id, mid_tick);
}

void PaperPortfolio::mark_liquidation(
    const std::string& asset_id,
    std::int64_t liquidation_tick
) {
    positions_.mark_liquidation(asset_id, liquidation_tick);
}

const PositionLedger& PaperPortfolio::positions() const noexcept {
    return positions_;
}

ExposureView PaperPortfolio::exposure() const {
    return positions_.exposure();
}

}  // namespace trading_engine::paper
