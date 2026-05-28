#include "chain_confirm/ClassifiedFillRecord.h"
#include "decode/public/NormalizedEventBatch.h"
#include "state/EntityStateStore.h"
#include "state/book/LOBWriter.h"
#include "state/core/MarketStateEventAdapter.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
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
using trading_engine::state::BookApplyCode;
using trading_engine::state::EntityStateStore;
using trading_engine::state::EntityStatus;
using trading_engine::state::LOBWriter;
using trading_engine::state::MarketStateEvent;
using trading_engine::state::StateApplyCode;
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

NormalizedEvent snapshot_event(std::uint64_t packet_id = 10) {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Snapshot;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = packet_id * 1000;
    event.market_id = kMarketId;
    event.asset_id = kAssetId;
    event.entity_id = kAssetId;
    event.raw_type = "book";
    event.bids = std::vector<BookLevel>{{0.50, 100.0}, {0.49, 200.0}};
    event.asks = std::vector<BookLevel>{{0.54, 150.0}, {0.55, 250.0}};
    return event;
}

NormalizedEvent delta_event(std::uint64_t packet_id = 11) {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Delta;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = packet_id * 1000;
    event.market_id = kMarketId;
    event.asset_id = kAssetId;
    event.entity_id = kAssetId;
    event.raw_type = "price_change";
    event.changes = std::vector<PriceLevelChange>{
        {NormalizedSide::Bid, 0.52, 50.0}
    };
    return event;
}

NormalizedEvent heartbeat_event(std::uint64_t packet_id = 12) {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Heartbeat;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = packet_id * 1000;
    event.raw_type = "PONG";
    return event;
}

NormalizedEventBatch batch_with(const NormalizedEvent& event) {
    NormalizedEventBatch batch;
    expect_true(batch.push_back(event), "batch push");
    return batch;
}

MarketStateEvent market_event_from(const NormalizedEvent& event) {
    const auto events = from_normalized_batch(batch_with(event));
    expect_equal(events.size(), 1ULL, "market event count");
    return events.front();
}

ClassifiedFillRecord classified_fill(bool removed = false) {
    ClassifiedFillRecord fill;
    fill.fill_id = "0xtx:1";
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
    fill.chain_seen_monotonic_ns = 9000;
    fill.source_sequence = 100;
    fill.removed = removed;
    return fill;
}

void LOBWriter_AppliesSnapshot() {
    EntityStateStore store;
    LOBWriter writer(&store);

    const auto result = writer.apply(market_event_from(snapshot_event()));

    expect_equal(result.code, BookApplyCode::Applied, "code");
    expect_equal(result.state_code, StateApplyCode::Applied, "state code");
    expect_true(result.ok(), "result ok");
    expect_true(store.initialized(kAssetId), "initialized");
    expect_equal(store.entity_count(), 1ULL, "entity count");
    expect_equal(store.snapshots_applied(), 1ULL, "snapshot count");
}

void LOBWriter_AppliesDeltaAfterSnapshot() {
    EntityStateStore store;
    LOBWriter writer(&store);

    writer.apply(market_event_from(snapshot_event()));
    const auto result = writer.apply(market_event_from(delta_event()));

    expect_equal(result.code, BookApplyCode::Applied, "code");
    expect_equal(result.state_code, StateApplyCode::Applied, "state code");
    expect_equal(store.deltas_applied(), 1ULL, "delta count");

    const auto* entity = store.get(kAssetId);
    expect_true(entity != nullptr, "entity exists");
    expect_true(entity->book.best_bid.has_value(), "best bid exists");
    expect_equal(entity->book.best_bid.value(), 0.52, "best bid");
}

void LOBWriter_RejectsDeltaBeforeSnapshot() {
    EntityStateStore store;
    LOBWriter writer(&store);

    const auto result = writer.apply(market_event_from(delta_event()));

    expect_equal(result.code, BookApplyCode::StateRejected, "code");
    expect_equal(
        result.state_code,
        StateApplyCode::DeltaBeforeSnapshot,
        "state code"
    );
    expect_false(result.ok(), "result ok");
    expect_equal(store.errors(), 1ULL, "errors");
    expect_equal(store.status(kAssetId), EntityStatus::Recovering, "status");
}

void LOBWriter_IgnoresHeartbeat() {
    EntityStateStore store;
    LOBWriter writer(&store);
    const auto before_hash = store.global_hash();

    const auto result = writer.apply(market_event_from(heartbeat_event()));

    expect_equal(result.code, BookApplyCode::IgnoredHeartbeat, "code");
    expect_equal(
        result.state_code,
        StateApplyCode::IgnoredHeartbeat,
        "state code"
    );
    expect_true(result.ok(), "result ok");
    expect_true(result.ignored(), "ignored");
    expect_equal(store.heartbeats_ignored(), 1ULL, "heartbeats ignored");
    expect_equal(store.entity_count(), 0ULL, "entity count");
    expect_equal(store.global_hash(), before_hash, "global hash");
}

void LOBWriter_IgnoresChainConfirmedFill() {
    EntityStateStore store;
    LOBWriter writer(&store);

    writer.apply(market_event_from(snapshot_event()));
    const auto before_hash = store.global_hash();
    const auto before_snapshot_count = store.snapshots_applied();
    const auto before_events_seen = store.events_seen();

    const auto result = writer.apply(
        from_classified_fill(classified_fill())
    );

    expect_equal(result.code, BookApplyCode::IgnoredChainEvent, "code");
    expect_true(result.ok(), "result ok");
    expect_true(result.ignored(), "ignored");
    expect_equal(store.global_hash(), before_hash, "global hash");
    expect_equal(
        store.snapshots_applied(),
        before_snapshot_count,
        "snapshot count"
    );
    expect_equal(store.events_seen(), before_events_seen, "events seen");
}

void LOBWriter_BatchCountsAreCorrect() {
    EntityStateStore store;
    LOBWriter writer(&store);

    std::vector<MarketStateEvent> events;
    events.push_back(market_event_from(snapshot_event(10)));
    events.push_back(market_event_from(delta_event(11)));
    events.push_back(market_event_from(heartbeat_event(12)));
    events.push_back(from_classified_fill(classified_fill()));

    const auto result = writer.apply_batch(events);

    expect_true(result.ok(), "batch ok");
    expect_equal(result.events_seen, 4ULL, "events seen");
    expect_equal(result.applied, 2ULL, "applied");
    expect_equal(result.ignored, 2ULL, "ignored");
    expect_equal(result.errors, 0ULL, "errors");
    expect_equal(result.state_changed, 2ULL, "state changed");
    expect_equal(result.results.size(), 4ULL, "result count");
    expect_equal(store.snapshots_applied(), 1ULL, "snapshots");
    expect_equal(store.deltas_applied(), 1ULL, "deltas");
    expect_equal(store.heartbeats_ignored(), 1ULL, "heartbeats");
    expect_equal(result.global_hash, store.global_hash(), "global hash");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"LOBWriter_AppliesSnapshot", &LOBWriter_AppliesSnapshot},
        {"LOBWriter_AppliesDeltaAfterSnapshot",
         &LOBWriter_AppliesDeltaAfterSnapshot},
        {"LOBWriter_RejectsDeltaBeforeSnapshot",
         &LOBWriter_RejectsDeltaBeforeSnapshot},
        {"LOBWriter_IgnoresHeartbeat", &LOBWriter_IgnoresHeartbeat},
        {"LOBWriter_IgnoresChainConfirmedFill",
         &LOBWriter_IgnoresChainConfirmedFill},
        {"LOBWriter_BatchCountsAreCorrect",
         &LOBWriter_BatchCountsAreCorrect}
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
