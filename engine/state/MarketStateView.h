#pragma once

#include "state/MarketStateQueryResult.h"
#include "state/MarketStateSnapshot.h"
#include "state/StateTypes.h"
#include "state/core/MarketStateStore.h"
#include "state/view/MarketDepthView.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace trading_engine::state {

class MarketStateView {
public:
    explicit MarketStateView(const MarketStateStore& store) noexcept;

    [[nodiscard]] bool exists(const std::string& entity_id) const;
    [[nodiscard]] bool exists(std::uint64_t entity_id) const;

    [[nodiscard]] StateQueryResult<PriceLevel> get_best_bid(
        const std::string& entity_id
    ) const;
    [[nodiscard]] StateQueryResult<PriceLevel> get_best_bid(
        std::uint64_t entity_id
    ) const;

    [[nodiscard]] StateQueryResult<PriceLevel> get_best_ask(
        const std::string& entity_id
    ) const;
    [[nodiscard]] StateQueryResult<PriceLevel> get_best_ask(
        std::uint64_t entity_id
    ) const;

    [[nodiscard]] StateQueryResult<BestBidAsk> get_bbo(
        const std::string& entity_id
    ) const;
    [[nodiscard]] StateQueryResult<BestBidAsk> get_bbo(
        std::uint64_t entity_id
    ) const;

    [[nodiscard]] StateQueryResult<std::int64_t> get_mid_tick(
        const std::string& entity_id
    ) const;
    [[nodiscard]] StateQueryResult<std::int64_t> get_mid_tick(
        std::uint64_t entity_id
    ) const;

    [[nodiscard]] StateQueryResult<std::int64_t> get_spread_tick(
        const std::string& entity_id
    ) const;
    [[nodiscard]] StateQueryResult<std::int64_t> get_spread_tick(
        std::uint64_t entity_id
    ) const;

    [[nodiscard]] StateQueryResult<MarketStateSnapshot> get_snapshot(
        const std::string& entity_id,
        std::uint32_t max_depth = kMaxSnapshotDepth
    ) const;
    [[nodiscard]] StateQueryResult<MarketStateSnapshot> get_snapshot(
        std::uint64_t entity_id,
        std::uint32_t max_depth = kMaxSnapshotDepth
    ) const;

    [[nodiscard]] std::uint16_t get_snapshots(
        std::span<const std::string* const> entity_ids,
        MarketStateSnapshot* out,
        std::uint16_t max_out
    ) const;

    [[nodiscard]] std::uint16_t get_depth_views(
        std::span<const std::string* const> entity_ids,
        std::span<const std::uint32_t> asset_indices,
        MarketDepthView* out,
        std::uint16_t max_out
    ) const;

    [[nodiscard]] DepthBatchReadResult read_depth_batch(
        std::span<const std::uint32_t> asset_indices
    ) const;

    [[nodiscard]] DepthBatchReadResult read_depth_batch_by_asset_id(
        std::span<const std::string_view> asset_ids
    ) const;

    [[nodiscard]] std::uint64_t state_hash(
        const std::string& entity_id
    ) const;
    [[nodiscard]] std::uint64_t state_hash(std::uint64_t entity_id) const;

    [[nodiscard]] std::uint64_t global_hash() const;

private:
    [[nodiscard]] StateQueryError executable_error(
        const MarketStateSnapshot& snapshot
    ) const noexcept;

private:
    const MarketStateStore& store_;
};

}  // namespace trading_engine::state
