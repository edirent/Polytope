#pragma once

#include "engine/risk/public/ApprovedQuote.h"
#include "engine/risk/public/QuoteRiskDecision.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/state/view/MarketDepthView.h"
#include "engine/strategy/market_making/public/QuoteIntent.h"

#include <cstdint>
#include <optional>

namespace trading_engine::risk {

using strategy::market_making::QuoteIntent;

struct QuoteRiskInput {
    const QuoteIntent* quote = nullptr;
    const state::MarketDepthView* depth = nullptr;
    const QuoteRiskPolicy* policy = nullptr;

    std::int64_t current_position_lots = 0;
    std::int64_t current_asset_exposure_tick = 0;
    std::uint32_t active_quotes_for_asset = 0;
    std::uint64_t last_replace_ts_ns = 0;

    std::uint64_t now_ns = 0;
};

struct QuoteRiskResult {
    QuoteRiskDecision decision;
    std::optional<ApprovedQuote> approved_quote;
};

}  // namespace trading_engine::risk
