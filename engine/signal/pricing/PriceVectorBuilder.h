#pragma once

#include "engine/signal/pricing/CostResult.h"
#include "engine/signal/reader/MarketSnapshotReader.h"
#include "engine/signal/pricing/SideResolver.h"

#include <array>
#include <cstdint>
#include <vector>

namespace trading_engine::signal {

struct PriceLeg {
    std::string asset_id;
    std::string market_id;

    Side side = Side::Buy;

    std::int64_t target_qty_lots = 0;

    ExecutableBookSide executable_side = ExecutableBookSide::Unsupported;

    const MarketStateSnapshot* snapshot = nullptr;
};

struct PriceVectorResult {
    bool ok = false;
    CostFailureReason failure_reason = CostFailureReason::None;
    std::uint16_t failed_leg_index = 0;
    std::uint16_t leg_count = 0;
    std::array<PriceLeg, kMaxIntentLegs> legs{};
};

class PriceVectorBuilder {
public:
    [[nodiscard]] PriceVectorResult build(
        const CandidateBundle& bundle,
        const std::vector<MarketStateSnapshot>& snapshots
    ) const;

private:
    SideResolver side_resolver_;
};

[[nodiscard]] PriceVectorResult build_price_vector(
    const CandidateBundle& bundle,
    const std::vector<MarketStateSnapshot>& snapshots
);

}  // namespace trading_engine::signal
