#include "state/core/MarketStateStore.h"

#include <utility>

namespace trading_engine::state {

MarketStateStore::MarketStateStore(StateRuntimeConfig runtime_config)
    : runtime_config_(runtime_config),
      shards_{LOBShard{0, runtime_config_}} {}

const StateRuntimeConfig& MarketStateStore::runtime_config() const noexcept {
    return runtime_config_;
}

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

std::uint16_t MarketStateStore::get_snapshots(
    std::span<const std::string* const> asset_ids,
    MarketStateSnapshot* out,
    std::uint16_t max_out
) const {
    if (ShardRouter::kNumShards == 1) {
        return shards_[0].snapshots(asset_ids, out, max_out);
    }

    std::uint16_t count = 0;
    for (const auto* asset_id : asset_ids) {
        if (!asset_id || count >= max_out) {
            continue;
        }
        auto snapshot = get_snapshot(*asset_id);
        if (snapshot.ok) {
            out[count++] = std::move(snapshot.value);
        }
    }
    return count;
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

std::uint64_t MarketStateStore::global_hash() const {
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
