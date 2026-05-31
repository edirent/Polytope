#include "decode/public/NormalizedEvent.h"
#include "state/EntityStateStore.h"
#include "state/StateHasher.h"
#include "state/hash/StateHashCache.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using trading_engine::decode::BookLevel;
using trading_engine::decode::NormalizedEvent;
using trading_engine::decode::NormalizedEventType;
using trading_engine::decode::NormalizedSide;
using trading_engine::decode::PriceLevelChange;
using trading_engine::state::EntityStateStore;
using trading_engine::state::StateHashCache;
using trading_engine::state::StateHasher;
using trading_engine::state::StateRuntimeConfig;

constexpr const char* kAssetId = "asset-hash-cache";
constexpr const char* kMarketId = "market-hash-cache";

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

template <typename Actual, typename Expected>
void expect_equal(
    const Actual& actual,
    const Expected& expected,
    const std::string& field
) {
    if (!(actual == expected)) {
        fail("mismatch: " + field);
    }
}

template <typename Actual, typename Expected>
void expect_not_equal(
    const Actual& actual,
    const Expected& expected,
    const std::string& field
) {
    if (actual == expected) {
        fail("unexpected equality: " + field);
    }
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

NormalizedEvent snapshot_event(std::uint64_t packet_id = 1) {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Snapshot;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = packet_id * 1'000;
    event.market_id = kMarketId;
    event.asset_id = kAssetId;
    event.entity_id = kAssetId;
    event.raw_type = "book";
    event.bids = std::vector<BookLevel>{{0.40, 100.0}};
    event.asks = std::vector<BookLevel>{{0.60, 100.0}};
    return event;
}

NormalizedEvent delta_event(std::uint64_t packet_id = 2) {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Delta;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = packet_id * 1'000;
    event.market_id = kMarketId;
    event.asset_id = kAssetId;
    event.entity_id = kAssetId;
    event.raw_type = "price_change";
    event.changes = std::vector<PriceLevelChange>{
        {NormalizedSide::Ask, 0.58, 120.0}
    };
    return event;
}

NormalizedEvent noop_delta_event(std::uint64_t packet_id = 3) {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Delta;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = packet_id * 1'000;
    event.market_id = kMarketId;
    event.asset_id = kAssetId;
    event.entity_id = kAssetId;
    event.raw_type = "price_change";
    return event;
}

EntityStateStore hot_store() {
    StateRuntimeConfig config;
    return EntityStateStore(config);
}

void StateHashCache_EntityHashMatchesStateHasher() {
    EntityStateStore store = hot_store();
    store.apply(snapshot_event());

    StateHashCache cache;
    cache.mark_dirty(kAssetId);

    const auto cached = cache.entity_hash(kAssetId, store);
    const auto* entity = store.get(kAssetId);
    expect_true(entity != nullptr, "entity exists");
    expect_equal(cached, StateHasher::hash_entity(*entity), "entity hash");
}

void StateHashCache_ReusesCleanEntityHash() {
    EntityStateStore store = hot_store();
    store.apply(snapshot_event());

    StateHashCache cache;
    cache.mark_dirty(kAssetId);
    (void)cache.take_access_stats();

    const auto first = cache.entity_hash(kAssetId, store);
    const auto first_stats = cache.take_access_stats();
    expect_equal(first_stats.hits, 0ULL, "first hits");
    expect_equal(first_stats.misses, 1ULL, "first misses");

    const auto second = cache.entity_hash(kAssetId, store);
    const auto second_stats = cache.take_access_stats();
    expect_equal(second, first, "cached entity hash");
    expect_equal(second_stats.hits, 1ULL, "second hits");
    expect_equal(second_stats.misses, 0ULL, "second misses");
}

void StateHashCache_MarkDirtyRecomputesEntityHash() {
    EntityStateStore store = hot_store();
    store.apply(snapshot_event());

    StateHashCache cache;
    cache.mark_dirty(kAssetId);
    const auto first = cache.entity_hash(kAssetId, store);

    store.apply(delta_event());
    expect_equal(
        cache.entity_hash(kAssetId, store),
        first,
        "cached hash before dirty"
    );

    cache.mark_dirty(kAssetId);
    const auto second = cache.entity_hash(kAssetId, store);
    expect_not_equal(second, first, "recomputed entity hash");
}

void StateHashCache_RecomputesDirtyEntityHash() {
    EntityStateStore store = hot_store();
    store.apply(snapshot_event());

    StateHashCache cache;
    cache.mark_dirty(kAssetId);
    const auto first = cache.entity_hash(kAssetId, store);
    (void)cache.take_access_stats();

    store.apply(delta_event());
    cache.mark_dirty(kAssetId);
    const auto second = cache.entity_hash(kAssetId, store);
    const auto stats = cache.take_access_stats();

    expect_not_equal(second, first, "dirty entity hash");
    expect_equal(stats.hits, 0ULL, "dirty hits");
    expect_equal(stats.misses, 1ULL, "dirty misses");
}

void StateHashCache_GlobalHashReturnsCachedUntilDirty() {
    EntityStateStore store = hot_store();
    store.apply(snapshot_event());

    StateHashCache cache;
    cache.mark_dirty(kAssetId);
    const auto first = cache.global_hash(store);

    store.apply(delta_event());
    expect_equal(cache.global_hash(store), first, "cached global hash");

    cache.mark_dirty(kAssetId);
    const auto second = cache.global_hash(store);
    expect_not_equal(second, first, "recomputed global hash");
}

void StateHashCache_RecomputesGlobalOnlyWhenDirty() {
    EntityStateStore store = hot_store();
    store.apply(snapshot_event());

    StateHashCache cache;
    cache.mark_dirty(kAssetId);
    const auto first = cache.global_hash(store);
    auto stats = cache.take_access_stats();
    expect_equal(stats.hits, 0ULL, "first global hits");
    expect_equal(stats.misses, 1ULL, "first global misses");

    const auto cached = cache.global_hash(store);
    stats = cache.take_access_stats();
    expect_equal(cached, first, "cached global hash");
    expect_equal(stats.hits, 1ULL, "cached global hits");
    expect_equal(stats.misses, 0ULL, "cached global misses");

    store.apply(delta_event());
    cache.mark_dirty(kAssetId);
    const auto second = cache.global_hash(store);
    stats = cache.take_access_stats();
    expect_not_equal(second, first, "dirty global hash");
    expect_equal(stats.hits, 0ULL, "dirty global hits");
    expect_equal(stats.misses, 1ULL, "dirty global misses");
}

void EntityStateStore_StateHashCacheDirtyOnMutation() {
    EntityStateStore store = hot_store();
    store.apply(snapshot_event());

    const auto first = store.state_hash(kAssetId);
    store.apply(noop_delta_event());
    expect_equal(store.state_hash(kAssetId), first, "noop keeps hash");

    store.apply(delta_event());
    const auto second = store.state_hash(kAssetId);
    expect_not_equal(second, first, "mutation refreshes entity hash");
}

void EntityStateStore_GlobalHashCacheDirtyOnMutation() {
    EntityStateStore store = hot_store();
    store.apply(snapshot_event());

    const auto first = store.global_hash();
    store.apply(noop_delta_event());
    expect_equal(store.global_hash(), first, "noop keeps global hash");

    store.apply(delta_event());
    const auto second = store.global_hash();
    expect_not_equal(second, first, "mutation refreshes global hash");
}

void StateHashCache_MissingEntityReturnsZero() {
    EntityStateStore store = hot_store();
    StateHashCache cache;

    cache.mark_dirty(kAssetId);
    expect_equal(cache.entity_hash(kAssetId, store), 0ULL, "missing entity");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"StateHashCache_EntityHashMatchesStateHasher",
         &StateHashCache_EntityHashMatchesStateHasher},
        {"StateHashCache_ReusesCleanEntityHash",
         &StateHashCache_ReusesCleanEntityHash},
        {"StateHashCache_MarkDirtyRecomputesEntityHash",
         &StateHashCache_MarkDirtyRecomputesEntityHash},
        {"StateHashCache_RecomputesDirtyEntityHash",
         &StateHashCache_RecomputesDirtyEntityHash},
        {"StateHashCache_GlobalHashReturnsCachedUntilDirty",
         &StateHashCache_GlobalHashReturnsCachedUntilDirty},
        {"StateHashCache_RecomputesGlobalOnlyWhenDirty",
         &StateHashCache_RecomputesGlobalOnlyWhenDirty},
        {"EntityStateStore_StateHashCacheDirtyOnMutation",
         &EntityStateStore_StateHashCacheDirtyOnMutation},
        {"EntityStateStore_GlobalHashCacheDirtyOnMutation",
         &EntityStateStore_GlobalHashCacheDirtyOnMutation},
        {"StateHashCache_MissingEntityReturnsZero",
         &StateHashCache_MissingEntityReturnsZero}
    };
    return test_map;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            fail("expected exactly one test name");
        }

        const auto it = tests().find(argv[1]);
        if (it == tests().end()) {
            fail(std::string("unknown test: ") + argv[1]);
        }

        it->second();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
