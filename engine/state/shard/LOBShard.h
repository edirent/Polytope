#pragma once

#include "state/EntityStateStore.h"
#include "state/MarketStateQueryResult.h"
#include "state/MarketStateSnapshot.h"
#include "state/book/LOBWriter.h"
#include "state/chain/ChainStateWriter.h"
#include "state/chain/SettlementStateWriter.h"
#include "state/core/MarketStateEvent.h"
#include "state/quality/BookQualityState.h"
#include "state/snapshot/SnapshotPublisher.h"

#include <cstdint>
#include <string>

namespace trading_engine::state {

class LOBShard {
public:
    explicit LOBShard(std::uint32_t shard_id);

    [[nodiscard]] std::uint32_t shard_id() const noexcept;

    StateApplyResult apply(const MarketStateEvent& event);

    [[nodiscard]] StateQueryResult<MarketStateSnapshot> snapshot(
        const std::string& asset_id
    ) const;

    [[nodiscard]] const ConfirmedTradeState* confirmed_trade_state(
        const std::string& asset_id
    ) const noexcept;

    [[nodiscard]] const SettlementState* settlement_state(
        const std::string& market_id
    ) const noexcept;

    [[nodiscard]] const BookQualityState& quality() const noexcept;
    [[nodiscard]] std::uint64_t book_hash() const noexcept;

private:
    StateApplyResult apply_book_event(const MarketStateEvent& event);
    StateApplyResult apply_chain_event(const MarketStateEvent& event);
    StateApplyResult apply_settlement_event(const MarketStateEvent& event);

    void publish_for_event(const MarketStateEvent& event);
    void publish_asset_snapshot(const std::string& asset_id);
    void refresh_quality(const std::string& asset_id);

    [[nodiscard]] std::string asset_id_for_event(
        const MarketStateEvent& event
    ) const;

    [[nodiscard]] std::string market_id_for_event(
        const MarketStateEvent& event
    ) const;

private:
    std::uint32_t shard_id_{0};

    EntityStateStore book_store_;
    LOBWriter lob_writer_;
    ChainStateWriter chain_writer_;
    SettlementStateWriter settlement_writer_;
    BookQualityState quality_;
    SnapshotPublisher snapshot_publisher_;
};

}  // namespace trading_engine::state
