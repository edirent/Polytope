#include "chain_confirm/ClassifiedFillRecord.h"
#include "decode/public/NormalizedEventBatch.h"
#include "state/core/MarketStateEventAdapter.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::chain_confirm::ClassifiedFillRecord;
using trading_engine::chain_confirm::ConfirmedDirection;
using trading_engine::chain_confirm::FillClassification;
using trading_engine::chain_confirm::FillMappingStatus;
using trading_engine::decode::NormalizedEvent;
using trading_engine::decode::NormalizedEventBatch;
using trading_engine::decode::NormalizedEventType;
using trading_engine::state::MarketStateEventType;
using trading_engine::state::from_classified_fill;
using trading_engine::state::from_normalized_batch;

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

NormalizedEvent ws_event(
    NormalizedEventType type,
    std::uint64_t packet_id = 42,
    std::uint64_t recv_monotonic_ns = 123456
) {
    NormalizedEvent event;
    event.event_type = type;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = recv_monotonic_ns;
    event.market_id = "market-a";
    event.asset_id = "asset-a";
    event.entity_id = "asset-a";
    return event;
}

NormalizedEventBatch batch_with(NormalizedEvent event) {
    NormalizedEventBatch batch;
    expect_true(batch.push_back(event), "batch push");
    return batch;
}

ClassifiedFillRecord classified_fill(
    FillClassification classification = FillClassification::ChainConfirmed,
    bool removed = false
) {
    ClassifiedFillRecord fill;
    fill.fill_id = "0xtx:7";
    fill.order_hash = "0xorder";
    fill.market_id = "market-a";
    fill.asset_id = "asset-a";
    fill.price_tick = 500000;
    fill.size_lots = 1000;
    fill.direction = ConfirmedDirection::BuyAggressor;
    fill.mapping_status = FillMappingStatus::Mapped;
    fill.classification = classification;
    fill.block_number = 1234;
    fill.tx_hash = "0xtx";
    fill.log_index = 7;
    fill.chain_seen_monotonic_ns = 987654;
    fill.source_sequence = 1234;
    fill.removed = removed;
    return fill;
}

void expect_single_ws_type(
    NormalizedEventType normalized_type,
    MarketStateEventType expected_type
) {
    const auto batch = batch_with(ws_event(normalized_type));
    const auto events = from_normalized_batch(batch);

    expect_equal(events.size(), 1ULL, "event count");
    expect_equal(events.front().type, expected_type, "event type");
}

void MarketStateEvent_NormalizedSnapshotToWsBookSnapshot() {
    expect_single_ws_type(
        NormalizedEventType::Snapshot,
        MarketStateEventType::WsBookSnapshot
    );
}

void MarketStateEvent_NormalizedDeltaToWsBookDelta() {
    expect_single_ws_type(
        NormalizedEventType::Delta,
        MarketStateEventType::WsBookDelta
    );
}

void MarketStateEvent_HeartbeatToWsHeartbeat() {
    expect_single_ws_type(
        NormalizedEventType::Heartbeat,
        MarketStateEventType::WsHeartbeat
    );
}

void MarketStateEvent_LifecycleToWsLifecycle() {
    expect_single_ws_type(
        NormalizedEventType::LifecycleEvent,
        MarketStateEventType::WsLifecycle
    );
}

void MarketStateEvent_ClassifiedConfirmedFillToChainConfirmedFill() {
    const auto fill = classified_fill();
    const auto event = from_classified_fill(fill);

    expect_equal(
        event.type,
        MarketStateEventType::ChainConfirmedFill,
        "event type"
    );
    expect_equal(event.chain_fill.fill_id, fill.fill_id, "fill id");
    expect_equal(event.chain_fill.direction, fill.direction, "direction");
}

void MarketStateEvent_RemovedFillToChainRemovedFill() {
    const auto fill = classified_fill(
        FillClassification::ChainRemoved,
        true
    );
    const auto event = from_classified_fill(fill);

    expect_equal(
        event.type,
        MarketStateEventType::ChainRemovedFill,
        "event type"
    );
    expect_true(event.chain_fill.removed, "removed");
}

void MarketStateEvent_MetadataPreserved() {
    const auto batch = batch_with(ws_event(
        NormalizedEventType::Snapshot,
        77,
        888999
    ));
    const auto ws_events = from_normalized_batch(batch);

    expect_equal(ws_events.size(), 1ULL, "ws event count");
    expect_equal(ws_events.front().market_id, "market-a", "ws market id");
    expect_equal(ws_events.front().asset_id, "asset-a", "ws asset id");
    expect_equal(
        ws_events.front().recv_monotonic_ns,
        888999ULL,
        "ws recv time"
    );
    expect_equal(
        ws_events.front().source_sequence,
        77ULL,
        "ws source sequence"
    );

    const auto fill = classified_fill();
    const auto chain_event = from_classified_fill(fill);

    expect_equal(
        chain_event.market_id,
        fill.market_id,
        "chain market id"
    );
    expect_equal(
        chain_event.asset_id,
        fill.asset_id,
        "chain asset id"
    );
    expect_equal(
        chain_event.recv_monotonic_ns,
        fill.chain_seen_monotonic_ns,
        "chain recv time"
    );
    expect_equal(
        chain_event.source_sequence,
        fill.source_sequence,
        "chain source sequence"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"MarketStateEvent_NormalizedSnapshotToWsBookSnapshot",
         &MarketStateEvent_NormalizedSnapshotToWsBookSnapshot},
        {"MarketStateEvent_NormalizedDeltaToWsBookDelta",
         &MarketStateEvent_NormalizedDeltaToWsBookDelta},
        {"MarketStateEvent_HeartbeatToWsHeartbeat",
         &MarketStateEvent_HeartbeatToWsHeartbeat},
        {"MarketStateEvent_LifecycleToWsLifecycle",
         &MarketStateEvent_LifecycleToWsLifecycle},
        {"MarketStateEvent_ClassifiedConfirmedFillToChainConfirmedFill",
         &MarketStateEvent_ClassifiedConfirmedFillToChainConfirmedFill},
        {"MarketStateEvent_RemovedFillToChainRemovedFill",
         &MarketStateEvent_RemovedFillToChainRemovedFill},
        {"MarketStateEvent_MetadataPreserved",
         &MarketStateEvent_MetadataPreserved}
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
