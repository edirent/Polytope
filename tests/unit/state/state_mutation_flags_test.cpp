#include "chain_confirm/ClassifiedFillRecord.h"
#include "decode/public/NormalizedEventBatch.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/StateHashPolicy.h"
#include "state/core/StateMutationFlags.h"
#include "state/shard/LOBShard.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using trading_engine::chain_confirm::ClassifiedFillRecord;
using trading_engine::chain_confirm::ConfirmedDirection;
using trading_engine::chain_confirm::FillClassification;
using trading_engine::chain_confirm::FillMappingStatus;
using trading_engine::decode::BookLevel;
using trading_engine::decode::NormalizedEvent;
using trading_engine::decode::NormalizedEventBatch;
using trading_engine::decode::NormalizedEventType;
using trading_engine::decode::NormalizedSide;
using trading_engine::decode::PriceLevelChange;
using trading_engine::state::LOBShard;
using trading_engine::state::MarketStateEvent;
using trading_engine::state::StateApplyCode;
using trading_engine::state::StateHashMode;
using trading_engine::state::StateMutationKind;
using trading_engine::state::StateRuntimeConfig;
using trading_engine::state::from_classified_fill;
using trading_engine::state::from_normalized_batch;

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

void expect_false(bool value, const std::string& field) {
    if (value) {
        fail("expected false: " + field);
    }
}

NormalizedEvent snapshot_event(std::uint64_t packet_id = 1) {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Snapshot;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = packet_id * 1'000'000'000ULL;
    event.market_id = kMarketId;
    event.asset_id = kAssetId;
    event.entity_id = kAssetId;
    event.raw_type = "book";
    event.bids = std::vector<BookLevel>{{0.50, 100.0}};
    event.asks = std::vector<BookLevel>{{0.54, 200.0}};
    return event;
}

NormalizedEvent delta_event(std::uint64_t packet_id = 2) {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Delta;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = packet_id * 1'000'000'000ULL;
    event.market_id = kMarketId;
    event.asset_id = kAssetId;
    event.entity_id = kAssetId;
    event.raw_type = "price_change";
    event.changes = std::vector<PriceLevelChange>{
        {NormalizedSide::Bid, 0.52, 50.0}
    };
    return event;
}

NormalizedEvent heartbeat_event(std::uint64_t packet_id = 3) {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Heartbeat;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = packet_id * 1'000'000'000ULL;
    event.raw_type = "PONG";
    return event;
}

MarketStateEvent market_event_from(const NormalizedEvent& event) {
    NormalizedEventBatch batch;
    expect_true(batch.push_back(event), "batch push");
    const auto events = from_normalized_batch(batch);
    expect_equal(events.size(), 1ULL, "event count");
    return events.front();
}

ClassifiedFillRecord fill_record(
    std::string fill_id,
    bool removed = false
) {
    ClassifiedFillRecord fill;
    fill.fill_id = std::move(fill_id);
    fill.order_hash = "0xorder";
    fill.market_id = kMarketId;
    fill.asset_id = kAssetId;
    fill.price_tick = 500000;
    fill.size_lots = 1000;
    fill.direction = ConfirmedDirection::BuyAggressor;
    fill.mapping_status = FillMappingStatus::Mapped;
    fill.classification = removed
        ? FillClassification::ChainRemoved
        : FillClassification::ChainConfirmed;
    fill.block_number = 100;
    fill.tx_hash = "0xtx";
    fill.log_index = 1;
    fill.chain_seen_monotonic_ns = 3'000'000'000ULL;
    fill.source_sequence = 100;
    fill.removed = removed;
    return fill;
}

void StateMutationFlags_SnapshotMarksBookChanged() {
    LOBShard shard(0);

    const auto result = shard.apply(market_event_from(snapshot_event()));

    expect_equal(result.code, StateApplyCode::Applied, "code");
    expect_true(result.state_changed, "state changed");
    expect_true(result.mutation.state_changed, "mutation state changed");
    expect_true(result.mutation.book_changed, "book changed");
    expect_true(result.mutation.publish_required, "publish required");
    expect_true(result.snapshot_published, "snapshot published");
    expect_equal(
        result.mutation.kind,
        StateMutationKind::BookSnapshot,
        "kind"
    );
    expect_equal(result.book_version, 1ULL, "book version");
    expect_true(result.snapshot_version_hash != 0, "snapshot version hash");
    expect_false(result.full_hash_computed, "full hash computed");
}

void StateMutationFlags_DeltaMarksBookChanged() {
    LOBShard shard(0);
    shard.apply(market_event_from(snapshot_event()));

    const auto result = shard.apply(market_event_from(delta_event()));

    expect_equal(result.code, StateApplyCode::Applied, "code");
    expect_true(result.state_changed, "state changed");
    expect_true(result.mutation.book_changed, "book changed");
    expect_true(result.mutation.publish_required, "publish required");
    expect_true(result.snapshot_published, "snapshot published");
    expect_equal(result.mutation.kind, StateMutationKind::BookDelta, "kind");
    expect_equal(result.book_version, 2ULL, "book version");
    expect_true(result.snapshot_version_hash != 0, "snapshot version hash");
}

void StateMutationFlags_DeltaMarksHashDirty() {
    StateRuntimeConfig config;
    config.hash_mode = StateHashMode::ReplayFull;
    LOBShard shard(0, config);
    shard.apply(market_event_from(snapshot_event()));

    const auto result = shard.apply(market_event_from(delta_event()));

    expect_true(result.mutation.book_changed, "book changed");
    expect_true(result.full_hash_computed, "full hash computed");
    expect_true(result.entity_hash != 0, "entity hash");
    expect_true(result.global_hash != 0, "global hash");
    expect_true(result.hash_cache_misses > 0, "hash cache misses");
}

void StateMutationFlags_TargetDeltaBeforeSnapshotIsBookMutation() {
    LOBShard shard(0);

    const auto result = shard.apply(market_event_from(delta_event()));

    expect_equal(result.code, StateApplyCode::DeltaBeforeSnapshot, "code");
    expect_true(result.state_changed, "state changed");
    expect_true(result.mutation.book_changed, "book changed");
    expect_true(result.mutation.publish_required, "publish required");
    expect_equal(result.mutation.kind, StateMutationKind::BookDelta, "kind");
    expect_equal(result.book_version, 2ULL, "book version");
}

void StateMutationFlags_HeartbeatDoesNotPublishOrHash() {
    LOBShard shard(0);

    const auto result = shard.apply(market_event_from(heartbeat_event()));

    expect_equal(result.code, StateApplyCode::IgnoredHeartbeat, "code");
    expect_false(result.state_changed, "state changed");
    expect_false(result.mutation.state_changed, "mutation state changed");
    expect_true(result.mutation.heartbeat_seen, "heartbeat seen");
    expect_false(result.mutation.publish_required, "publish required");
    expect_false(result.snapshot_published, "snapshot published");
    expect_equal(result.snapshot_publish_ns, 0ULL, "snapshot publish ns");
    expect_false(result.mutation.book_changed, "book changed");
    expect_equal(result.mutation.kind, StateMutationKind::Heartbeat, "kind");
    expect_equal(result.entity_hash, 0ULL, "entity hash");
    expect_equal(result.global_hash, 0ULL, "global hash");
    expect_false(result.full_hash_computed, "full hash computed");
    expect_equal(result.hash_entity_ns, 0ULL, "hash entity ns");
    expect_equal(result.hash_global_ns, 0ULL, "hash global ns");
    expect_equal(result.hash_cache_hits, 0ULL, "hash cache hits");
    expect_equal(result.hash_cache_misses, 0ULL, "hash cache misses");
}

void StateMutationFlags_ChainFillMarksChainChanged() {
    LOBShard shard(0);
    shard.apply(market_event_from(snapshot_event()));

    const auto result = shard.apply(
        from_classified_fill(fill_record("fill-1"))
    );

    expect_equal(result.code, StateApplyCode::Noop, "code");
    expect_true(result.state_changed, "state changed");
    expect_true(result.mutation.chain_changed, "chain changed");
    expect_false(result.mutation.book_changed, "book changed");
    expect_true(result.mutation.publish_required, "publish required");
    expect_true(result.snapshot_published, "snapshot published");
    expect_equal(result.mutation.kind, StateMutationKind::ChainFill, "kind");
    expect_true(result.chain_version > 0, "chain version");
    expect_true(result.snapshot_version_hash != 0, "snapshot version hash");
}

void StateMutationFlags_ChainRemovedFillMarksChainChanged() {
    LOBShard shard(0);
    shard.apply(market_event_from(snapshot_event()));
    shard.apply(from_classified_fill(fill_record("fill-remove")));

    const auto result = shard.apply(
        from_classified_fill(fill_record("fill-remove", true))
    );

    expect_equal(result.code, StateApplyCode::Noop, "code");
    expect_true(result.state_changed, "state changed");
    expect_true(result.mutation.chain_changed, "chain changed");
    expect_false(result.mutation.book_changed, "book changed");
    expect_true(result.mutation.publish_required, "publish required");
    expect_true(result.snapshot_published, "snapshot published");
    expect_equal(
        result.mutation.kind,
        StateMutationKind::ChainRemovedFill,
        "kind"
    );
    expect_true(result.chain_version > 0, "chain version");
}

void StateMutationFlags_HotPathSnapshotPublishesVersionHashOnly() {
    LOBShard shard(0);

    const auto result = shard.apply(market_event_from(snapshot_event()));
    const auto snapshot = shard.snapshot(kAssetId);

    expect_true(snapshot.ok, "snapshot ok");
    expect_false(result.full_hash_computed, "full hash computed");
    expect_equal(snapshot.value.state_hash, 0ULL, "legacy book hash");
    expect_equal(
        snapshot.value.snapshot_version_hash,
        result.snapshot_version_hash,
        "snapshot version hash"
    );
    expect_true(
        snapshot.value.snapshot_version_hash != 0,
        "nonzero snapshot version hash"
    );
}

void StateMutationFlags_ReplayFullSnapshotPublishesFullBookHashFromResult() {
    StateRuntimeConfig config;
    config.hash_mode = StateHashMode::ReplayFull;
    LOBShard shard(0, config);

    const auto result = shard.apply(market_event_from(snapshot_event()));
    const auto snapshot = shard.snapshot(kAssetId);

    expect_true(snapshot.ok, "snapshot ok");
    expect_true(result.full_hash_computed, "full hash computed");
    expect_true(result.entity_hash != 0, "entity hash");
    expect_equal(
        snapshot.value.state_hash,
        result.entity_hash,
        "legacy book hash"
    );
    expect_equal(
        snapshot.value.snapshot_version_hash,
        result.snapshot_version_hash,
        "snapshot version hash"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"StateMutationFlags_SnapshotMarksBookChanged",
         &StateMutationFlags_SnapshotMarksBookChanged},
        {"StateMutationFlags_DeltaMarksBookChanged",
         &StateMutationFlags_DeltaMarksBookChanged},
        {"StateMutationFlags_DeltaMarksHashDirty",
         &StateMutationFlags_DeltaMarksHashDirty},
        {"StateMutationFlags_TargetDeltaBeforeSnapshotIsBookMutation",
         &StateMutationFlags_TargetDeltaBeforeSnapshotIsBookMutation},
        {"StateMutationFlags_HeartbeatDoesNotPublishOrHash",
         &StateMutationFlags_HeartbeatDoesNotPublishOrHash},
        {"StateMutationFlags_ChainFillMarksChainChanged",
         &StateMutationFlags_ChainFillMarksChainChanged},
        {"StateMutationFlags_ChainRemovedFillMarksChainChanged",
         &StateMutationFlags_ChainRemovedFillMarksChainChanged},
        {"StateMutationFlags_HotPathSnapshotPublishesVersionHashOnly",
         &StateMutationFlags_HotPathSnapshotPublishesVersionHashOnly},
        {"StateMutationFlags_ReplayFullSnapshotPublishesFullBookHashFromResult",
         &StateMutationFlags_ReplayFullSnapshotPublishesFullBookHashFromResult}
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
