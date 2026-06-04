#pragma once

#include "engine/strategy/market_making/fair/FairValueModel.h"
#include "engine/strategy/market_making/core/MarketMakingWorkflow.h"
#include "engine/strategy/market_making/public/MarketMakingConfig.h"
#include "engine/strategy/market_making/public/MarketMakingResult.h"
#include "engine/strategy/market_making/quote/InventorySkewModel.h"
#include "engine/strategy/market_making/quote/QuoteEngine.h"
#include "engine/strategy/market_making/quote/QuoteSizeModel.h"
#include "engine/strategy/market_making/quote/SpreadModel.h"
#include "engine/strategy/market_making/refresh/QuoteRefreshPolicy.h"
#include "engine/strategy/market_making/state/QuoteBook.h"

#include <cstdint>

namespace trading_engine::strategy::market_making {

class MarketMakingEngine {
public:
    explicit MarketMakingEngine(MarketMakingConfig config = {});

    [[nodiscard]] MarketMakingResult on_market_update(
        const MarketMakingInput& input
    );

    [[nodiscard]] const QuoteBook& quote_book() const noexcept;
    bool remove_active_quote(std::uint32_t asset_index);

private:
    MarketMakingConfig config_;
    QuoteBook quote_book_;
    FairValueModel fair_value_model_;
    SpreadModel spread_model_;
    InventorySkewModel inventory_skew_model_;
    QuoteSizeModel quote_size_model_;
    QuoteEngine quote_engine_;
    QuoteRefreshPolicy refresh_policy_;
};

}  // namespace trading_engine::strategy::market_making
