#include "state/core/MarketStateStore.h"

namespace trading_engine::state {

MarketStateStore::MarketStateStore()
    : shards_{LOBShard{0}} {}

StateApplyResult MarketStateStore::apply(const MarketStateEvent& event) {
    const std::string asset_id =
        !event.asset_id.empty() ? event.asset_id :
        !event.ws_event.asset_id.empty() ? event.ws_event.asset_id :
        !event.ws_event.entity_id.empty() ? event.ws_event.entity_id :
        event.chain_fill.asset_id;

    return shards_[shard_for_asset(asset_id)].apply(event);
}

StateQueryResult<MarketStateSnapshot> MarketStateStore::get_snapshot(
    const std::string& asset_id
) const {
    return shards_[shard_for_asset(asset_id)].snapshot(asset_id);
}

bool MarketStateStore::exists(const std::string& asset_id) const {
    return get_snapshot(asset_id).ok;
}

std::uint64_t MarketStateStore::state_hash(
    const std::string& asset_id
) const {
    const auto snapshot = get_snapshot(asset_id);
    return snapshot.ok ? snapshot.value.state_hash : 0;
}

std::uint64_t MarketStateStore::global_hash() const noexcept {
    std::uint64_t hash = 0;
    for (const auto& shard : shards_) {
        hash ^= shard.book_hash();
    }
    return hash;
}

std::uint32_t MarketStateStore::shard_for_asset(
    const std::string& asset_id
) const noexcept {
    return router_.shard_for_asset(asset_id);
}

}  // namespace trading_engine::state
