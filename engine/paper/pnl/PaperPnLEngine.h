#pragma once

#include "engine/paper/ledger/CashLedger.h"
#include "engine/paper/pnl/EquityCurve.h"
#include "engine/paper/pnl/MarkPriceProvider.h"
#include "engine/paper/portfolio/PaperPortfolio.h"
#include "engine/state/view/MarketDepthView.h"

#include <cstdint>
#include <span>

namespace trading_engine::paper {

struct PaperPnLResult {
    EquityCurve equity;

    std::int64_t realized_pnl_tick = 0;
    std::int64_t unrealized_pnl_mid_tick = 0;
    std::int64_t unrealized_pnl_liquidation_tick = 0;

    MarkQuality worst_mark_quality = MarkQuality::Good;
};

class PaperPnLEngine {
public:
    explicit PaperPnLEngine(MarkPriceProvider mark_provider = {});

    [[nodiscard]] PaperPnLResult compute(
        const PaperPortfolio& portfolio,
        const CashLedger& cash,
        std::span<const trading_engine::state::MarketDepthView> depth_views,
        std::uint64_t ts_ns = 0
    ) const;

private:
    MarkPriceProvider mark_provider_;
};

}  // namespace trading_engine::paper
