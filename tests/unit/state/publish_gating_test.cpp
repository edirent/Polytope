#include "decode/public/NormalizedEventBatch.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/MarketStateStore.h"

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
using trading_engine::decode::NormalizedSide;
using trading_engine::decode::PriceLevelChange;
using trading_engine::state::MarketStateEvent;
using trading_engine::state::MarketStateEventType;
using trading_engine::state::MarketStateStore;
using trading_engine::state::StateApplyCode;
using trading_engine::state::StateRuntimeConfig;
using trading_engine::state::from_normalized_batch;

constexpr const char* kAssetId = "asset-publish-gating";
constexpr const char* kMarketId = "market-publish-gating";

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
    event.bids = std::vector<BookLevel>{{0.45, 100.0}};
    event.asks = std::vector<BookLevel>{{0.55, 100.0}};
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
        {NormalizedSide::Ask, 0.54, 200.0}
    };
    return event;
}

NormalizedEvent heartbeat_event(std::uint64_t packet_id = 3) {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Heartbeat;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = packet_id * 1'000;
    event.raw_type = "PONG";
    return event;
}

MarketStateEvent market_event_from(const NormalizedEvent& event) {
    NormalizedEventBatch batch;
    expect_true(batch.push_back(event), "batch push");
    const auto events = from_normalized_batch(batch);
    expect_equal(events.size(), 1ULL, "market state event count");
    return events.front();
}

MarketStateEvent data_quality_event() {
    MarketStateEvent event;
    event.type = MarketStateEventType::DataQualityUpdate;
    event.market_id = kMarketId;
    event.asset_id = kAssetId;
    event.recv_monotonic_ns = 4'000;
    event.source_sequence = 4;
    return event;
}

void PublishGating_SnapshotPublishes() {
    MarketStateStore store;

    const auto result = store.apply(market_event_from(snapshot_event()));

    expect_true(result.ok(), "apply ok");
    expect_true(result.mutation.publish_required, "publish required");
    expect_true(result.snapshot_published, "snapshot published");
    expect_true(result.snapshot_build_ns > 0, "snapshot build ns");
    expect_true(result.snapshot_publish_ns > 0, "snapshot publish ns");
}

void PublishGating_DeltaRequiresPublish() {
    MarketStateStore store;
    store.apply(market_event_from(snapshot_event()));

    const auto result = store.apply(market_event_from(delta_event()));

    expect_true(result.ok(), "apply ok");
    expect_true(result.mutation.book_changed, "book changed");
    expect_true(result.mutation.publish_required, "publish required");
    expect_true(result.snapshot_published, "snapshot published");
}

void PublishGating_HeartbeatDoesNotPublish() {
    StateRuntimeConfig config;
    config.publish_on_heartbeat = true;
    MarketStateStore store(config);
    store.apply(market_event_from(snapshot_event()));

    const auto result = store.apply(market_event_from(heartbeat_event()));

    expect_equal(result.code, StateApplyCode::IgnoredHeartbeat, "code");
    expect_false(result.state_changed, "state changed");
    expect_true(result.mutation.heartbeat_seen, "heartbeat seen");
    expect_false(result.mutation.publish_required, "publish required");
    expect_false(result.snapshot_published, "snapshot published");
    expect_equal(result.snapshot_build_ns, 0ULL, "snapshot build ns");
    expect_equal(result.snapshot_publish_ns, 0ULL, "snapshot publish ns");
}

void PublishGating_QualityNoChangeDoesNotPublish() {
    MarketStateStore store;
    store.apply(market_event_from(snapshot_event()));

    const auto result = store.apply(data_quality_event());

    expect_true(result.ok(), "apply ok");
    expect_false(result.state_changed, "state changed");
    expect_false(result.mutation.quality_changed, "quality changed");
    expect_false(result.mutation.publish_required, "publish required");
    expect_false(result.snapshot_published, "snapshot published");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"PublishGating_SnapshotPublishes", &PublishGating_SnapshotPublishes},
        {"PublishGating_DeltaRequiresPublish",
         &PublishGating_DeltaRequiresPublish},
        {"PublishGating_HeartbeatDoesNotPublish",
         &PublishGating_HeartbeatDoesNotPublish},
        {"PublishGating_QualityNoChangeDoesNotPublish",
         &PublishGating_QualityNoChangeDoesNotPublish}
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
