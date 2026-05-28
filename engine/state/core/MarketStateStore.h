#pragma once

#include "state/MarketStateQueryResult.h"
#include "state/MarketStateSnapshot.h"
#include "state/core/MarketStateEvent.h"
#include "state/shard/LOBShard.h"
#include "state/shard/ShardRouter.h"

#include <array>
#include <cstdint>
#include <string>

namespace trading_engine::state {

class MarketStateStore {
public:
    MarketStateStore();

    StateApplyResult apply(const MarketStateEvent& event);

    [[nodiscard]] StateQueryResult<MarketStateSnapshot> get_snapshot(
        const std::string& asset_id
    ) const;

    [[nodiscard]] bool exists(const std::string& asset_id) const;

    [[nodiscard]] std::uint64_t state_hash(
        const std::string& asset_id
    ) const;

    [[nodiscard]] std::uint64_t global_hash() const noexcept;

private:
    [[nodiscard]] std::uint32_t shard_for_asset(
        const std::string& asset_id
    ) const noexcept;

private:
    std::array<LOBShard, ShardRouter::kNumShards> shards_;
    ShardRouter router_;
};

}  // namespace trading_engine::state
