#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace trading_engine::state {

class EntityStateStore;

struct EntityHashCacheEntry {
    std::uint64_t entity_hash{0};
    std::uint64_t book_version{0};
    std::uint64_t chain_version{0};
    std::uint64_t quality_version{0};
    bool dirty{true};
};

struct HashCacheAccessStats {
    std::uint64_t hits{0};
    std::uint64_t misses{0};
};

class StateHashCache {
public:
    void mark_dirty(const std::string& entity_id);
    void clear();

    [[nodiscard]] HashCacheAccessStats take_access_stats();

    [[nodiscard]] std::uint64_t entity_hash(
        const std::string& entity_id,
        const EntityStateStore& store
    );

    [[nodiscard]] std::uint64_t global_hash(
        const EntityStateStore& store
    );

private:
    std::unordered_map<std::string, EntityHashCacheEntry> entity_hashes_;
    std::uint64_t global_hash_{0};
    bool global_dirty_{true};
    HashCacheAccessStats access_stats_;
};

}  // namespace trading_engine::state
