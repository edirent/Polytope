#include "decode/public/NormalizedEvent.h"
#include "state/EntityStateStore.h"
#include "state/chain/SettlementState.h"
#include "state/chain/SettlementStateWriter.h"
#include "state/core/GlobalBitmaskState.h"

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
using trading_engine::state::GlobalBitmaskState;
using trading_engine::state::SettlementState;
using trading_engine::state::SettlementStateWriter;
using trading_engine::state::SettlementStatus;

constexpr const char* kMarketId = "market-a";
constexpr const char* kAssetId = "asset-a";

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

NormalizedEvent snapshot_event() {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Snapshot;
    event.packet_id = 1;
    event.recv_monotonic_ns = 1000;
    event.market_id = kMarketId;
    event.asset_id = kAssetId;
    event.entity_id = kAssetId;
    event.raw_type = "book";
    event.bids = std::vector<BookLevel>{{0.50, 100.0}};
    event.asks = std::vector<BookLevel>{{0.54, 100.0}};
    return event;
}

void Settlement_StartsOpen() {
    SettlementState state;

    expect_equal(state.status, SettlementStatus::Open, "status");
    expect_equal(state.resolved, false, "resolved");
    expect_equal(state.winning_asset_id, std::string{}, "winning asset");
    expect_equal(state.last_update_block, 0ULL, "last block");
    expect_equal(state.version, 0ULL, "version");
}

void Settlement_MarksClosed() {
    SettlementStateWriter writer;

    const auto result = writer.mark_closed(kMarketId, 123);
    expect_true(result.ok(), "apply ok");

    const auto* state = writer.get(kMarketId);
    expect_true(state != nullptr, "state exists");
    expect_equal(state->status, SettlementStatus::Closed, "status");
    expect_equal(state->resolved, false, "resolved");
    expect_equal(state->last_update_block, 123ULL, "last block");
    expect_equal(state->version, 1ULL, "version");
}

void Settlement_MarksResolved() {
    SettlementStateWriter writer;

    const auto result = writer.mark_resolved(kMarketId, kAssetId, 456);
    expect_true(result.ok(), "apply ok");

    const auto* state = writer.get(kMarketId);
    expect_true(state != nullptr, "state exists");
    expect_equal(state->status, SettlementStatus::Resolved, "status");
    expect_equal(state->resolved, true, "resolved");
    expect_equal(state->last_update_block, 456ULL, "last block");
    expect_equal(state->version, 1ULL, "version");
}

void ResolvedState_StoresWinningAsset() {
    SettlementStateWriter writer;
    writer.mark_resolved(kMarketId, kAssetId, 456);

    const auto* state = writer.get(kMarketId);
    expect_true(state != nullptr, "state exists");
    expect_equal(
        state->winning_asset_id,
        std::string{kAssetId},
        "winning asset"
    );
}

void GlobalBitmask_DefaultsZero() {
    GlobalBitmaskState state;

    expect_equal(state.resolved_true_mask, 0ULL, "true mask");
    expect_equal(state.resolved_false_mask, 0ULL, "false mask");
    expect_equal(state.invalid_mask, 0ULL, "invalid mask");
    expect_equal(state.version, 0ULL, "version");
}

void Settlement_DoesNotMutateOrderBook() {
    EntityStateStore store;
    const auto apply_result = store.apply(snapshot_event());
    expect_true(apply_result.ok(), "snapshot apply");

    const auto before_hash = store.global_hash();
    const auto before_events_seen = store.events_seen();
    const auto before_snapshots = store.snapshots_applied();

    SettlementStateWriter writer;
    writer.mark_resolved(kMarketId, kAssetId, 456);

    expect_equal(store.global_hash(), before_hash, "global hash");
    expect_equal(store.events_seen(), before_events_seen, "events seen");
    expect_equal(store.snapshots_applied(), before_snapshots, "snapshots");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"Settlement_StartsOpen", &Settlement_StartsOpen},
        {"Settlement_MarksClosed", &Settlement_MarksClosed},
        {"Settlement_MarksResolved", &Settlement_MarksResolved},
        {"ResolvedState_StoresWinningAsset",
         &ResolvedState_StoresWinningAsset},
        {"GlobalBitmask_DefaultsZero", &GlobalBitmask_DefaultsZero},
        {"Settlement_DoesNotMutateOrderBook",
         &Settlement_DoesNotMutateOrderBook}
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

    int failures = 0;
    for (const auto& [name, _] : tests()) {
        failures += run_test(name) == 0 ? 0 : 1;
    }

    return failures == 0 ? 0 : 1;
}
