#pragma once

#include "state/EntityStateStore.h"
#include "state/MarketStateQueryResult.h"
#include "state/MarketStateSnapshot.h"
#include "state/book/LOBWriter.h"
#include "state/chain/ChainStateWriter.h"
#include "state/chain/SettlementStateWriter.h"
#include "state/core/MarketStateEvent.h"
#include "state/core/StateHashPolicy.h"
#include "state/quality/BookQualityState.h"
#include "state/snapshot/SnapshotPublisher.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

namespace trading_engine::state {

struct SnapshotBuildContext {
    std::string asset_id;

    std::uint64_t book_version{0};
    std::uint64_t chain_version{0};
    std::uint64_t quality_version{0};
    std::uint64_t snapshot_version_hash{0};

    std::optional<std::uint64_t> full_entity_hash;
};

class LOBShard {
public:
    explicit LOBShard(
        std::uint32_t shard_id,
        StateRuntimeConfig runtime_config = {}
    );

    [[nodiscard]] std::uint32_t shard_id() const noexcept;
    [[nodiscard]] const StateRuntimeConfig& runtime_config() const noexcept;

    StateApplyResult apply(const MarketStateEvent& event);

    [[nodiscard]] StateQueryResult<MarketStateSnapshot> snapshot(
        const std::string& asset_id
    ) const;

    [[nodiscard]] std::uint16_t snapshots(
        std::span<const std::string* const> asset_ids,
        MarketStateSnapshot* out,
        std::uint16_t max_out
    ) const;

    [[nodiscard]] std::uint16_t depth_views(
        std::span<const std::string* const> asset_ids,
        std::span<const std::uint32_t> asset_indices,
        MarketDepthView* out,
        std::uint16_t max_out
    ) const;

    [[nodiscard]] const ConfirmedTradeState* confirmed_trade_state(
        const std::string& asset_id
    ) const noexcept;

    [[nodiscard]] const SettlementState* settlement_state(
        const std::string& market_id
    ) const noexcept;

    [[nodiscard]] const BookQualityState& quality() const noexcept;
    [[nodiscard]] std::uint64_t book_hash() const;

private:
    StateApplyResult apply_book_event(const MarketStateEvent& event);
    StateApplyResult apply_chain_event(const MarketStateEvent& event);
    StateApplyResult apply_settlement_event(const MarketStateEvent& event);

    [[nodiscard]] bool should_publish_after_apply(
        const MarketStateEvent& event,
        const StateApplyResult& result
    ) const noexcept;

    [[nodiscard]] bool publish_asset_snapshot(
        const SnapshotBuildContext& context
    );
    void refresh_quality(const std::string& asset_id);

    [[nodiscard]] SnapshotBuildContext build_snapshot_context(
        const MarketStateEvent& event,
        const StateApplyResult& result
    ) const;

    [[nodiscard]] std::string asset_id_for_event(
        const MarketStateEvent& event
    ) const;

    [[nodiscard]] std::string market_id_for_event(
        const MarketStateEvent& event
    ) const;

private:
    std::uint32_t shard_id_{0};
    StateRuntimeConfig runtime_config_;
    std::uint64_t quality_version_{0};
    std::unordered_map<std::string, std::uint64_t> cached_legacy_book_hash_;

    EntityStateStore book_store_;
    LOBWriter lob_writer_;
    ChainStateWriter chain_writer_;
    SettlementStateWriter settlement_writer_;
    BookQualityState quality_;
    SnapshotPublisher snapshot_publisher_;
};

}  // namespace trading_engine::state
