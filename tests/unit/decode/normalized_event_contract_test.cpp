#include "decode/public/NormalizedEvent.h"
#include "decode/public/NormalizedEventBatch.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::decode::BookLevel;
using trading_engine::decode::NormalizedEvent;
using trading_engine::decode::NormalizedEventBatch;
using trading_engine::decode::NormalizedEventType;
using trading_engine::decode::NormalizedSide;
using trading_engine::decode::PriceLevelChange;
using trading_engine::decode::SourceId;

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

void NormalizedEvent_DefaultConstructsSafely() {
    const NormalizedEvent event;

    expect_equal(event.packet_id, 0ULL, "packet_id");
    expect_equal(event.recv_wall_ns, 0ULL, "recv_wall_ns");
    expect_equal(event.recv_monotonic_ns, 0ULL, "recv_monotonic_ns");
    expect_equal(event.source_id, SourceId::Unknown, "source_id");
    expect_equal(event.event_type, NormalizedEventType::Unknown, "event_type");
    expect_true(event.raw_type.empty(), "raw_type");
    expect_true(event.entity_id.empty(), "entity_id");
    expect_true(event.asset_id.empty(), "asset_id");
    expect_true(event.market_id.empty(), "market_id");
    expect_true(event.condition_id.empty(), "condition_id");
    expect_equal(event.event_ts, 0ULL, "event_ts");
    expect_true(event.bids.empty(), "bids");
    expect_true(event.asks.empty(), "asks");
    expect_true(event.changes.empty(), "changes");
    expect_false(event.best_bid.has_value(), "best_bid");
    expect_false(event.best_ask.has_value(), "best_ask");
    expect_false(event.tick_size.has_value(), "tick_size");
    expect_true(event.winning_asset_id.empty(), "winning_asset_id");
    expect_true(event.warnings.empty(), "warnings");
}

void NormalizedEvent_HeartbeatHasStableFields() {
    NormalizedEvent event;
    event.packet_id = 7;
    event.source_id = SourceId::PolymarketMarket;
    event.event_type = NormalizedEventType::Heartbeat;
    event.raw_type = "PONG";

    expect_equal(event.packet_id, 7ULL, "packet_id");
    expect_equal(event.source_id, SourceId::PolymarketMarket, "source_id");
    expect_equal(event.event_type, NormalizedEventType::Heartbeat, "event_type");
    expect_equal(event.raw_type, std::string{"PONG"}, "raw_type");
    expect_true(event.entity_id.empty(), "entity_id");
    expect_true(event.bids.empty(), "bids");
    expect_true(event.asks.empty(), "asks");
    expect_true(event.changes.empty(), "changes");
}

void NormalizedEvent_SnapshotHasStableEntityId() {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Snapshot;
    event.entity_id = "asset_x";
    event.asset_id = "asset_x";
    event.market_id = "market_x";
    event.bids.push_back(BookLevel{0.42, 10.0});
    event.asks.push_back(BookLevel{0.58, 11.0});

    expect_equal(event.event_type, NormalizedEventType::Snapshot, "event_type");
    expect_equal(event.entity_id, std::string{"asset_x"}, "entity_id");
    expect_equal(event.asset_id, std::string{"asset_x"}, "asset_id");
    expect_equal(event.market_id, std::string{"market_x"}, "market_id");
    expect_equal(event.bids.size(), 1U, "bids size");
    expect_equal(event.asks.size(), 1U, "asks size");
}

void NormalizedEvent_DeltaHasStableEntityId() {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Delta;
    event.entity_id = "asset_x";
    event.asset_id = "asset_x";
    event.changes.push_back(
        PriceLevelChange{NormalizedSide::Bid, 0.41, 12.0}
    );

    expect_equal(event.event_type, NormalizedEventType::Delta, "event_type");
    expect_equal(event.entity_id, std::string{"asset_x"}, "entity_id");
    expect_equal(event.asset_id, std::string{"asset_x"}, "asset_id");
    expect_equal(event.changes.size(), 1U, "changes size");
    expect_equal(
        event.changes.front().side,
        NormalizedSide::Bid,
        "change side"
    );
}

void NormalizedEventBatch_CanHoldMultipleEvents() {
    NormalizedEventBatch batch;

    NormalizedEvent snapshot;
    snapshot.event_type = NormalizedEventType::Snapshot;
    NormalizedEvent delta;
    delta.event_type = NormalizedEventType::Delta;

    expect_true(batch.push_back(snapshot), "push snapshot");
    expect_true(batch.push_back(delta), "push delta");
    expect_equal(batch.size(), 2U, "size");
    expect_false(batch.overflowed, "overflowed");
    expect_equal(
        batch.events[0].event_type,
        NormalizedEventType::Snapshot,
        "first event"
    );
    expect_equal(
        batch.events[1].event_type,
        NormalizedEventType::Delta,
        "second event"
    );
}

void NormalizedEventBatch_RejectsOrCapsOverflow() {
    NormalizedEventBatch batch;
    NormalizedEvent event;

    for (std::size_t i = 0; i < NormalizedEventBatch::kMaxEvents; ++i) {
        expect_true(batch.push_back(event), "push before cap");
    }

    expect_false(batch.push_back(event), "push after cap");
    expect_equal(
        batch.size(),
        NormalizedEventBatch::kMaxEvents,
        "capped size"
    );
    expect_true(batch.overflowed, "overflowed");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"NormalizedEvent_DefaultConstructsSafely",
         &NormalizedEvent_DefaultConstructsSafely},
        {"NormalizedEvent_HeartbeatHasStableFields",
         &NormalizedEvent_HeartbeatHasStableFields},
        {"NormalizedEvent_SnapshotHasStableEntityId",
         &NormalizedEvent_SnapshotHasStableEntityId},
        {"NormalizedEvent_DeltaHasStableEntityId",
         &NormalizedEvent_DeltaHasStableEntityId},
        {"NormalizedEventBatch_CanHoldMultipleEvents",
         &NormalizedEventBatch_CanHoldMultipleEvents},
        {"NormalizedEventBatch_RejectsOrCapsOverflow",
         &NormalizedEventBatch_RejectsOrCapsOverflow}
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
