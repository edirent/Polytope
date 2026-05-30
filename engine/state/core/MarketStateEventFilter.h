#pragma once

#include "state/core/MarketStateEvent.h"
#include "state/core/StateUniverse.h"

#include <cstdint>
#include <string>

namespace trading_engine::state {

enum class MarketStateEventFilterDecision : std::uint8_t {
    Pass,
    Filter
};

enum class MarketStateEventFilterReason : std::uint8_t {
    None,

    PairedAssetNotInUniverse,
    AssetNotInUniverse,
    MarketNotInUniverse,
    MissingAssetId,
    MissingMarketId
};

struct MarketStateEventFilterResult {
    MarketStateEventFilterDecision decision{
        MarketStateEventFilterDecision::Pass
    };
    MarketStateEventFilterReason reason{
        MarketStateEventFilterReason::None
    };

    std::string asset_id;
    std::string market_id;

    [[nodiscard]] bool passed() const noexcept {
        return decision == MarketStateEventFilterDecision::Pass;
    }
};

class MarketStateEventFilter {
public:
    explicit MarketStateEventFilter(const StateUniverse& universe);

    [[nodiscard]] MarketStateEventFilterResult filter(
        const MarketStateEvent& event
    ) const;

private:
    const StateUniverse* universe_;
};

[[nodiscard]] const char* to_string(
    MarketStateEventFilterDecision decision
) noexcept;

[[nodiscard]] const char* to_string(
    MarketStateEventFilterReason reason
) noexcept;

}  // namespace trading_engine::state
