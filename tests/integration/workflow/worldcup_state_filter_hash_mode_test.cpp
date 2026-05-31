#include "decode/public/NormalizedEventBatch.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/MarketStateEventFilter.h"
#include "state/core/MarketStateStore.h"
#include "state/core/StateUniverse.h"

#include <cstdint>
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
using trading_engine::state::MarketStateEventFilter;
using trading_engine::state::MarketStateEventFilterReason;
using trading_engine::state::MarketStateStore;
using trading_engine::state::StateHashMode;
using trading_engine::state::StateRuntimeConfig;
using trading_engine::state::StateUniverse;
using trading_engine::state::from_normalized_batch;

constexpr const char* kWorldCupMarketId = "event-30615-worldcup-winner";
constexpr const char* kActiveAssetId = "worldcup-spain-yes";
constexpr const char* kPairedAssetId = "worldcup-spain-no";

struct WorkflowSummary {
    std::uint64_t events_seen{0};
    std::uint64_t events_passed{0};
    std::uint64_t filtered_paired_asset{0};
    std::uint64_t state_errors{0};
    std::uint64_t full_hash_computed{0};
    std::uint64_t full_hash_skipped{0};
    std::uint64_t final_global_hash{0};
};

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

NormalizedEvent snapshot_event(
    std::string asset_id,
    std::uint64_t packet_id
) {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Snapshot;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = packet_id * 1'000;
    event.market_id = kWorldCupMarketId;
    event.asset_id = asset_id;
    event.entity_id = event.asset_id;
    event.raw_type = "book";
    event.bids = std::vector<BookLevel>{{0.30, 100.0}};
    event.asks = std::vector<BookLevel>{{0.33, 100.0}};
    return event;
}

NormalizedEvent delta_event(
    std::string asset_id,
    std::uint64_t packet_id
) {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Delta;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = packet_id * 1'000;
    event.market_id = kWorldCupMarketId;
    event.asset_id = asset_id;
    event.entity_id = event.asset_id;
    event.raw_type = "price_change";
    event.changes = std::vector<PriceLevelChange>{
        {NormalizedSide::Bid, 0.31, 50.0}
    };
    return event;
}

NormalizedEvent heartbeat_event(std::uint64_t packet_id) {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Heartbeat;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = packet_id * 1'000;
    event.raw_type = "PONG";
    return event;
}

std::vector<MarketStateEvent> market_events_from(
    const std::vector<NormalizedEvent>& normalized_events
) {
    NormalizedEventBatch batch;
    for (const auto& event : normalized_events) {
        expect_true(batch.push_back(event), "batch push");
    }
    return from_normalized_batch(batch);
}

StateUniverse worldcup_universe() {
    StateUniverse universe;
    universe.active_asset_ids.insert(kActiveAssetId);
    universe.active_market_ids.insert(kWorldCupMarketId);
    return universe;
}

WorkflowSummary run_workflow(StateHashMode mode) {
    StateRuntimeConfig config;
    config.hash_mode = mode;

    MarketStateStore store(config);
    const StateUniverse universe = worldcup_universe();
    MarketStateEventFilter filter(universe);
    WorkflowSummary summary;

    const auto events = market_events_from({
        snapshot_event(kActiveAssetId, 1),
        delta_event(kPairedAssetId, 2),
        heartbeat_event(3),
        delta_event(kActiveAssetId, 4)
    });

    for (const auto& event : events) {
        ++summary.events_seen;
        const auto filter_result = filter.filter(event);
        if (!filter_result.passed()) {
            if (filter_result.reason ==
                MarketStateEventFilterReason::PairedAssetNotInUniverse) {
                ++summary.filtered_paired_asset;
            }
            continue;
        }

        ++summary.events_passed;
        const auto result = store.apply(event);
        if (!result.ok()) {
            ++summary.state_errors;
        }
        if (result.full_hash_computed) {
            ++summary.full_hash_computed;
        } else {
            ++summary.full_hash_skipped;
        }
    }

    summary.final_global_hash = store.global_hash();
    return summary;
}

void WorldCupStateFilterHashMode_FiltersPairedAssetAcrossHashModes() {
    const auto hot = run_workflow(StateHashMode::HotPathLight);
    const auto replay = run_workflow(StateHashMode::ReplayFull);
    const auto debug = run_workflow(StateHashMode::DebugVerify);

    for (const auto& summary : {hot, replay, debug}) {
        expect_equal(summary.events_seen, 4ULL, "events_seen");
        expect_equal(summary.events_passed, 3ULL, "events_passed");
        expect_equal(
            summary.filtered_paired_asset,
            1ULL,
            "filtered paired asset"
        );
        expect_equal(summary.state_errors, 0ULL, "state_errors");
        expect_true(summary.final_global_hash != 0, "final global hash");
    }

    expect_equal(hot.full_hash_computed, 0ULL, "hot full hash computed");
    expect_true(replay.full_hash_computed > 0, "replay full hash computed");
    expect_true(debug.full_hash_computed > 0, "debug full hash computed");
    expect_equal(
        hot.final_global_hash,
        replay.final_global_hash,
        "hot replay hash"
    );
    expect_equal(
        replay.final_global_hash,
        debug.final_global_hash,
        "replay debug hash"
    );
}

}  // namespace

int main() {
    try {
        WorldCupStateFilterHashMode_FiltersPairedAssetAcrossHashModes();
        std::cout
            << "WorldCupStateFilterHashMode_FiltersPairedAssetAcrossHashModes"
            << " passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "WorldCupStateFilterHashMode_FiltersPairedAssetAcrossHashModes"
            << " failed: " << error.what() << '\n';
        return 1;
    }
}
