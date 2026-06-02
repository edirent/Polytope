#pragma once

#include "engine/strategy/market_making/state/ActiveQuoteState.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace trading_engine::strategy::market_making {

class QuoteBook {
public:
    bool upsert(const ActiveQuoteState& quote);
    bool remove(std::uint32_t asset_index);

    [[nodiscard]] const ActiveQuoteState* find(
        std::uint32_t asset_index
    ) const;
    [[nodiscard]] ActiveQuoteState* find(std::uint32_t asset_index);

    [[nodiscard]] std::vector<ActiveQuoteState> active_quotes() const;
    [[nodiscard]] std::uint64_t hash() const noexcept;

private:
    std::unordered_map<std::uint32_t, ActiveQuoteState> quotes_by_asset_;
};

}  // namespace trading_engine::strategy::market_making
