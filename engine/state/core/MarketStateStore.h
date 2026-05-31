#pragma once

#include "state/MarketStateQueryResult.h"
#include "state/MarketStateSnapshot.h"
#include "state/core/MarketStateEvent.h"
#include "state/core/StateHashPolicy.h"
#include "state/shard/LOBShard.h"
#include "state/shard/ShardRouter.h"
#include "state/view/MarketDepthView.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace trading_engine::state {

class MarketStateStore {
public:
    explicit MarketStateStore(StateRuntimeConfig runtime_config = {});

    [[nodiscard]] const StateRuntimeConfig& runtime_config() const noexcept;

    StateApplyResult apply(const MarketStateEvent& event);

    [[nodiscard]] StateQueryResult<MarketStateSnapshot> get_snapshot(
        const std::string& asset_id
    ) const;

    [[nodiscard]] std::uint16_t get_snapshots(
        std::span<const std::string* const> asset_ids,
        MarketStateSnapshot* out,
        std::uint16_t max_out
    ) const;

    [[nodiscard]] std::uint16_t get_depth_views(
        std::span<const std::string* const> asset_ids,
        std::span<const std::uint32_t> asset_indices,
        MarketDepthView* out,
        std::uint16_t max_out
    ) const;

    [[nodiscard]] bool exists(const std::string& asset_id) const;

    [[nodiscard]] std::uint64_t state_hash(
        const std::string& asset_id
    ) const;

    [[nodiscard]] std::uint64_t global_hash() const;

private:
    [[nodiscard]] std::uint32_t shard_for_asset(
        const std::string& asset_id
    ) const noexcept;

private:
    StateRuntimeConfig runtime_config_;
    std::array<LOBShard, ShardRouter::kNumShards> shards_;
    ShardRouter router_;
};

}  // namespace trading_engine::state
