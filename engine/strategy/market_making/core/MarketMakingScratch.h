#pragma once

#include "engine/strategy/market_making/public/CancelQuoteIntent.h"
#include "engine/strategy/market_making/public/QuoteIntent.h"

#include <array>
#include <cstdint>

namespace trading_engine::strategy::market_making {

struct MarketMakingScratch {
    std::array<QuoteIntent, 2> quotes{};
    std::array<CancelQuoteIntent, 2> cancels{};
    std::uint16_t quote_count = 0;
    std::uint16_t cancel_count = 0;

    void reset() noexcept {
        quote_count = 0;
        cancel_count = 0;
    }
};

}  // namespace trading_engine::strategy::market_making
