#include "decode/public/NormalizedEvent.h"
#include "state/EntityStateStore.h"
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
using trading_engine::decode::NormalizedEventType;
using trading_engine::state::EntityStateStore;
using trading_engine::state::StateHashMode;
using trading_engine::state::StateRuntimeConfig;

constexpr const char* kAssetId = "asset-hash-policy";
constexpr const char* kMarketId = "market-hash-policy";

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

NormalizedEvent heartbeat_event(std::uint64_t packet_id = 2) {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Heartbeat;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = packet_id * 1'000;
    event.raw_type = "PONG";
    return event;
}

EntityStateStore store_for(StateHashMode mode) {
    StateRuntimeConfig config;
    config.hash_mode = mode;
    return EntityStateStore(config);
}

void StateHashPolicy_DefaultRuntimeIsHotPathLight() {
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

void StateHashPolicy_HotPathSnapshotSkipsFullHash() {
    auto store = store_for(StateHashMode::HotPathLight);

    const auto result = store.apply(snapshot_event());

    expect_true(result.ok(), "apply ok");
    expect_false(result.full_hash_computed, "full hash computed");
    expect_equal(result.entity_hash, 0ULL, "entity hash");
    expect_equal(result.global_hash, 0ULL, "global hash");
    expect_true(result.snapshot_version_hash != 0, "snapshot version hash");
}

void StateHashPolicy_ReplayFullSnapshotComputesFullHash() {
    auto store = store_for(StateHashMode::ReplayFull);

    const auto result = store.apply(snapshot_event());

    expect_true(result.ok(), "apply ok");
    expect_true(result.full_hash_computed, "full hash computed");
    expect_true(result.entity_hash != 0, "entity hash");
    expect_equal(result.global_hash, store.global_hash(), "global hash");
}

void StateHashPolicy_DebugVerifySnapshotComputesFullHash() {
    auto store = store_for(StateHashMode::DebugVerify);

    const auto result = store.apply(snapshot_event());

    expect_true(result.ok(), "apply ok");
    expect_true(result.full_hash_computed, "full hash computed");
    expect_equal(
        result.entity_hash,
        store.debug_recomputed_state_hash(kAssetId),
        "entity hash"
    );
    expect_equal(
        result.global_hash,
        store.debug_recomputed_global_hash(),
        "global hash"
    );
}

void StateHashPolicy_HeartbeatSkipsFullHashInHotPath() {
    auto store = store_for(StateHashMode::HotPathLight);

    const auto result = store.apply(heartbeat_event());

    expect_true(result.ok(), "apply ok");
    expect_false(result.state_changed, "state changed");
    expect_true(result.mutation.heartbeat_seen, "heartbeat seen");
    expect_false(result.full_hash_computed, "full hash computed");
    expect_equal(result.entity_hash, 0ULL, "entity hash");
    expect_equal(result.global_hash, 0ULL, "global hash");
    expect_equal(result.hash_entity_ns, 0ULL, "hash entity ns");
    expect_equal(result.hash_global_ns, 0ULL, "hash global ns");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"StateHashPolicy_DefaultRuntimeIsHotPathLight",
         &StateHashPolicy_DefaultRuntimeIsHotPathLight},
        {"StateHashPolicy_HotPathSnapshotSkipsFullHash",
         &StateHashPolicy_HotPathSnapshotSkipsFullHash},
        {"StateHashPolicy_ReplayFullSnapshotComputesFullHash",
         &StateHashPolicy_ReplayFullSnapshotComputesFullHash},
        {"StateHashPolicy_DebugVerifySnapshotComputesFullHash",
         &StateHashPolicy_DebugVerifySnapshotComputesFullHash},
        {"StateHashPolicy_HeartbeatSkipsFullHashInHotPath",
         &StateHashPolicy_HeartbeatSkipsFullHashInHotPath}
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
