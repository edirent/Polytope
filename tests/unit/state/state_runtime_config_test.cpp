#include "decode/public/NormalizedEventBatch.h"
#include "state/EntityStateStore.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/MarketStateStore.h"
#include "state/core/StateHashPolicy.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using trading_engine::decode::BookLevel;
using trading_engine::decode::NormalizedEvent;
using trading_engine::decode::NormalizedEventBatch;
using trading_engine::decode::NormalizedEventType;
using trading_engine::state::EntityStateStore;
using trading_engine::state::MarketStateStore;
using trading_engine::state::StateHashMode;
using trading_engine::state::StateRuntimeConfig;
using trading_engine::state::from_normalized_batch;

constexpr const char* kAssetId = "asset-runtime-config";
constexpr const char* kMarketId = "market-runtime-config";

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

void expect_false(bool value, const std::string& field) {
    if (value) {
        fail("expected false: " + field);
    }
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

NormalizedEvent snapshot_event() {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Snapshot;
    event.packet_id = 1;
    event.recv_monotonic_ns = 1'000;
    event.market_id = kMarketId;
    event.asset_id = kAssetId;
    event.entity_id = kAssetId;
    event.raw_type = "book";
    event.bids = std::vector<BookLevel>{{0.10, 10.0}};
    event.asks = std::vector<BookLevel>{{0.20, 20.0}};
    return event;
}

NormalizedEvent noop_delta_event() {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Delta;
    event.packet_id = 2;
    event.recv_monotonic_ns = 2'000;
    event.market_id = kMarketId;
    event.asset_id = kAssetId;
    event.entity_id = kAssetId;
    event.raw_type = "price_change";
    return event;
}

NormalizedEvent heartbeat_event() {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Heartbeat;
    event.packet_id = 3;
    event.recv_monotonic_ns = 3'000;
    event.raw_type = "PONG";
    return event;
}

trading_engine::state::StateApplyResult apply_one(
    MarketStateStore& store,
    const NormalizedEvent& event
) {
    NormalizedEventBatch batch;
    expect_true(batch.push_back(event), "batch push");
    const auto events = from_normalized_batch(batch);
    expect_equal(events.size(), 1ULL, "event count");
    const auto result = store.apply(events.front());
    expect_true(result.ok(), "state apply ok");
    return result;
}

void StateRuntimeConfig_DefaultsHotPathLight() {
    const StateRuntimeConfig config;

    expect_equal(
        config.hash_mode,
        StateHashMode::HotPathLight,
        "hash mode"
    );
    expect_false(config.publish_on_noop, "publish_on_noop");
    expect_false(config.publish_on_heartbeat, "publish_on_heartbeat");
    expect_false(
        config.compute_full_hash_on_publish,
        "compute_full_hash_on_publish"
    );
}

void MarketStateStore_DefaultRuntimeConfigIsHotPathLight() {
    const MarketStateStore store;
    const auto& config = store.runtime_config();

    expect_equal(
        config.hash_mode,
        StateHashMode::HotPathLight,
        "store hash mode"
    );
    expect_false(config.publish_on_noop, "store publish_on_noop");
    expect_false(config.publish_on_heartbeat, "store publish_on_heartbeat");
}

void MarketStateStore_AcceptsReplayFullConfig() {
    StateRuntimeConfig config;
    config.hash_mode = StateHashMode::ReplayFull;
    config.publish_on_noop = true;
    config.publish_on_heartbeat = true;
    config.compute_full_hash_on_publish = true;

    const MarketStateStore store(config);
    const auto& applied = store.runtime_config();

    expect_equal(applied.hash_mode, StateHashMode::ReplayFull, "hash mode");
    expect_true(applied.publish_on_noop, "publish_on_noop");
    expect_true(applied.publish_on_heartbeat, "publish_on_heartbeat");
    expect_true(
        applied.compute_full_hash_on_publish,
        "compute_full_hash_on_publish"
    );
}

void MarketStateStore_DefaultDoesNotPublishNoop() {
    MarketStateStore store;
    apply_one(store, snapshot_event());
    const auto before = store.get_snapshot(kAssetId);
    expect_true(before.ok, "before snapshot");

    const auto result = apply_one(store, noop_delta_event());
    const auto after = store.get_snapshot(kAssetId);
    expect_true(after.ok, "after snapshot");

    expect_false(result.snapshot_published, "snapshot published");
    expect_equal(after.value.version, before.value.version, "version stable");
}

void MarketStateStore_HotPathLightDoesNotComputeApplyFullHash() {
    MarketStateStore store;

    const auto result = apply_one(store, snapshot_event());

    expect_false(result.full_hash_computed, "full hash computed");
    expect_equal(result.entity_hash, 0ULL, "entity hash");
    expect_equal(result.global_hash, 0ULL, "global hash");
    expect_true(result.cheap_fingerprint != 0, "cheap fingerprint");
    expect_true(store.global_hash() != 0, "explicit full global hash");
}

void EntityStateStore_DefaultKeepsReplayFullCompatibility() {
    EntityStateStore store;

    const auto result = store.apply(snapshot_event());

    expect_true(result.ok(), "state apply ok");
    expect_equal(
        store.runtime_config().hash_mode,
        StateHashMode::ReplayFull,
        "entity store hash mode"
    );
    expect_true(result.full_hash_computed, "full hash computed");
    expect_true(result.entity_hash != 0, "entity hash");
    expect_equal(result.global_hash, store.global_hash(), "global hash");
}

void EntityStateStore_ReplayFullSkipsFullHashWhenUnchanged() {
    EntityStateStore store;
    store.apply(snapshot_event());

    const auto noop = store.apply(noop_delta_event());
    const auto heartbeat = store.apply(heartbeat_event());

    expect_false(noop.full_hash_computed, "noop full hash computed");
    expect_equal(noop.entity_hash, 0ULL, "noop entity hash");
    expect_equal(noop.global_hash, 0ULL, "noop global hash");
    expect_true(noop.snapshot_version_hash != 0, "noop snapshot hash");

    expect_false(heartbeat.full_hash_computed, "heartbeat full hash computed");
    expect_equal(heartbeat.entity_hash, 0ULL, "heartbeat entity hash");
    expect_equal(heartbeat.global_hash, 0ULL, "heartbeat global hash");
}

void MarketStateStore_DoesNotPublishNoopEvenWhenConfigured() {
    StateRuntimeConfig config;
    config.publish_on_noop = true;

    MarketStateStore store(config);
    apply_one(store, snapshot_event());
    const auto before = store.get_snapshot(kAssetId);
    expect_true(before.ok, "before snapshot");

    const auto result = apply_one(store, noop_delta_event());
    const auto after = store.get_snapshot(kAssetId);
    expect_true(after.ok, "after snapshot");

    expect_false(result.snapshot_published, "snapshot published");
    expect_equal(after.value.version, before.value.version, "version stable");
}

void MarketStateStore_DoesNotPublishHeartbeatEvenWhenConfigured() {
    StateRuntimeConfig config;
    config.publish_on_heartbeat = true;

    MarketStateStore store(config);
    apply_one(store, snapshot_event());
    const auto before = store.get_snapshot(kAssetId);
    expect_true(before.ok, "before snapshot");

    const auto result = apply_one(store, heartbeat_event());
    const auto after = store.get_snapshot(kAssetId);
    expect_true(after.ok, "after snapshot");

    expect_false(result.snapshot_published, "snapshot published");
    expect_equal(after.value.version, before.value.version, "version stable");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"StateRuntimeConfig_DefaultsHotPathLight",
         &StateRuntimeConfig_DefaultsHotPathLight},
        {"MarketStateStore_DefaultRuntimeConfigIsHotPathLight",
         &MarketStateStore_DefaultRuntimeConfigIsHotPathLight},
        {"MarketStateStore_AcceptsReplayFullConfig",
         &MarketStateStore_AcceptsReplayFullConfig},
        {"MarketStateStore_DefaultDoesNotPublishNoop",
         &MarketStateStore_DefaultDoesNotPublishNoop},
        {"MarketStateStore_HotPathLightDoesNotComputeApplyFullHash",
         &MarketStateStore_HotPathLightDoesNotComputeApplyFullHash},
        {"EntityStateStore_DefaultKeepsReplayFullCompatibility",
         &EntityStateStore_DefaultKeepsReplayFullCompatibility},
        {"EntityStateStore_ReplayFullSkipsFullHashWhenUnchanged",
         &EntityStateStore_ReplayFullSkipsFullHashWhenUnchanged},
        {"MarketStateStore_DoesNotPublishNoopEvenWhenConfigured",
         &MarketStateStore_DoesNotPublishNoopEvenWhenConfigured},
        {"MarketStateStore_DoesNotPublishHeartbeatEvenWhenConfigured",
         &MarketStateStore_DoesNotPublishHeartbeatEvenWhenConfigured}
    };
    return test_map;
}

int run_test(const std::string& name) {
    const auto it = tests().find(name);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << name << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << name << " failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << name << " passed\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        return run_test(argv[1]);
    }

    std::cerr << "usage: state_runtime_config_tests <test_name>\n";
    return 2;
}
