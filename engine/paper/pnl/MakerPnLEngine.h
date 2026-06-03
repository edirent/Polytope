#pragma once

#include "engine/paper/ledger/PaperLedger.h"
#include "engine/paper/pnl/MakerPnLTypes.h"
#include "engine/paper/pnl/MarkPriceProvider.h"
#include "engine/state/view/MarketDepthView.h"

#include <cstdint>
#include <span>

namespace trading_engine::paper {

class MakerPnLEngine {
public:
    explicit MakerPnLEngine(MarkPriceProvider mark_provider = {});

    [[nodiscard]] MakerPnLSnapshot compute(
        const PaperLedger& ledger,
        std::span<const trading_engine::state::MarketDepthView> depth_views,
        std::uint64_t ts_ns = 0
    ) const;

private:
    MarkPriceProvider mark_provider_;
};

}  // namespace trading_engine::paper
