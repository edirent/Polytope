#include "state/hash/StateHashCache.h"

#include "state/EntityStateStore.h"
#include "state/StateHasher.h"

namespace trading_engine::state {

void StateHashCache::mark_dirty(const std::string& entity_id) {
    global_dirty_ = true;

    if (entity_id.empty()) {
        return;
    }

    entity_hashes_[entity_id].dirty = true;
}

void StateHashCache::clear() {
    entity_hashes_.clear();
    global_hash_ = 0;
    global_dirty_ = true;
    access_stats_ = {};
}

HashCacheAccessStats StateHashCache::take_access_stats() {
    const HashCacheAccessStats stats = access_stats_;
    access_stats_ = {};
    return stats;
}

std::uint64_t StateHashCache::entity_hash(
    const std::string& entity_id,
    const EntityStateStore& store
) {
    const auto* entity = store.get(entity_id);
    if (!entity) {
        entity_hashes_.erase(entity_id);
        ++access_stats_.misses;
        return 0;
    }

    const auto existing = entity_hashes_.find(entity_id);
    if (existing != entity_hashes_.end() && !existing->second.dirty) {
        ++access_stats_.hits;
        return existing->second.entity_hash;
    }

    ++access_stats_.misses;
    auto& entry = entity_hashes_[entity_id];
    entry.entity_hash = StateHasher::hash_entity(*entity);
    entry.book_version = entity->last_packet_id;
    entry.chain_version = 0;
    entry.quality_version = 0;
    entry.dirty = false;
    return entry.entity_hash;
}

std::uint64_t StateHashCache::global_hash(
    const EntityStateStore& store
) {
    if (!global_dirty_) {
        ++access_stats_.hits;
        return global_hash_;
    }

    ++access_stats_.misses;
    global_hash_ = StateHasher::hash_entity_map(store.entities_);
    global_dirty_ = false;
    return global_hash_;
}

}  // namespace trading_engine::state
