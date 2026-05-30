#include "decode/public/NormalizedEvent.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/MarketStateEventFilter.h"
#include "state/core/MarketStateStore.h"
#include "state/core/StateUniverse.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

using trading_engine::decode::NormalizedEvent;
using trading_engine::decode::NormalizedEventType;
using trading_engine::decode::NormalizedSide;
using trading_engine::decode::PriceLevelChange;
using trading_engine::state::MarketStateEvent;
using trading_engine::state::MarketStateEventFilter;
using trading_engine::state::MarketStateEventFilterDecision;
using trading_engine::state::MarketStateEventFilterReason;
using trading_engine::state::MarketStateStore;
using trading_engine::state::StateApplyCode;
using trading_engine::state::StateUniverse;
using trading_engine::state::from_normalized_batch;

constexpr const char* kActiveMarket = "market-active";
constexpr const char* kOtherMarket = "market-other";
constexpr const char* kActiveAsset = "asset-active";
constexpr const char* kPairedAsset = "asset-paired";
constexpr const char* kExternalAsset = "asset-external";

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

StateUniverse universe() {
    StateUniverse out;
    out.active_asset_ids.insert(kActiveAsset);
    out.active_market_ids.insert(kActiveMarket);
    return out;
}

NormalizedEvent ws_event(
    NormalizedEventType type,
    std::string asset_id,
    std::string market_id = kActiveMarket
) {
    NormalizedEvent event;
    event.event_type = type;
    event.packet_id = 7;
    event.recv_monotonic_ns = 7000;
    event.market_id = std::move(market_id);
    event.asset_id = asset_id;
    event.entity_id = std::move(asset_id);
    event.raw_type = type == NormalizedEventType::Snapshot
        ? "book"
        : "price_change";
    if (type == NormalizedEventType::Snapshot) {
        event.bids.push_back({0.50, 10.0});
        event.asks.push_back({0.55, 10.0});
    } else if (type == NormalizedEventType::Delta) {
        event.changes.push_back(
            PriceLevelChange{NormalizedSide::Bid, 0.51, 5.0}
        );
    }
    return event;
}

MarketStateEvent state_event(const NormalizedEvent& event) {
    trading_engine::decode::NormalizedEventBatch batch;
    expect_true(batch.push_back(event), "batch push");
    const auto events = from_normalized_batch(batch);
    expect_equal(events.size(), 1ULL, "state event count");
    return events.front();
}

MarketStateEvent heartbeat_event() {
    return state_event(ws_event(NormalizedEventType::Heartbeat, {}));
}

MarketStateEvent lifecycle_event(std::string market_id) {
    return state_event(ws_event(
        NormalizedEventType::LifecycleEvent,
        {},
        std::move(market_id)
    ));
}

void expect_filter(
    const MarketStateEvent& event,
    MarketStateEventFilterDecision decision,
    MarketStateEventFilterReason reason
) {
    const auto active_universe = universe();
    const MarketStateEventFilter filter(active_universe);
    const auto result = filter.filter(event);
    expect_equal(result.decision, decision, "decision");
    expect_equal(result.reason, reason, "reason");
}

void MarketStateEventFilter_PassesActiveAssetSnapshot() {
    expect_filter(
        state_event(ws_event(NormalizedEventType::Snapshot, kActiveAsset)),
        MarketStateEventFilterDecision::Pass,
        MarketStateEventFilterReason::None
    );
}

void MarketStateEventFilter_PassesActiveAssetDelta() {
    expect_filter(
        state_event(ws_event(NormalizedEventType::Delta, kActiveAsset)),
        MarketStateEventFilterDecision::Pass,
        MarketStateEventFilterReason::None
    );
}

void MarketStateEventFilter_FiltersPairedAssetDelta() {
    expect_filter(
        state_event(ws_event(NormalizedEventType::Delta, kPairedAsset)),
        MarketStateEventFilterDecision::Filter,
        MarketStateEventFilterReason::PairedAssetNotInUniverse
    );
}

void MarketStateEventFilter_FiltersExternalAsset() {
    expect_filter(
        state_event(ws_event(
            NormalizedEventType::Delta,
            kExternalAsset,
            kOtherMarket
        )),
        MarketStateEventFilterDecision::Filter,
        MarketStateEventFilterReason::AssetNotInUniverse
    );
}

void MarketStateEventFilter_PassesHeartbeat() {
    expect_filter(
        heartbeat_event(),
        MarketStateEventFilterDecision::Pass,
        MarketStateEventFilterReason::None
    );
}

void MarketStateEventFilter_PassesLifecycleForActiveMarket() {
    expect_filter(
        lifecycle_event(kActiveMarket),
        MarketStateEventFilterDecision::Pass,
        MarketStateEventFilterReason::None
    );
}

void MarketStateEventFilter_FiltersLifecycleForInactiveMarket() {
    expect_filter(
        lifecycle_event(kOtherMarket),
        MarketStateEventFilterDecision::Filter,
        MarketStateEventFilterReason::MarketNotInUniverse
    );
}

void PairedAssetDeltaIsFilteredBeforeStateStore() {
    const auto event =
        state_event(ws_event(NormalizedEventType::Delta, kPairedAsset));
    const auto active_universe = universe();
    const MarketStateEventFilter filter(active_universe);
    const auto decision = filter.filter(event);
    expect_equal(
        decision.reason,
        MarketStateEventFilterReason::PairedAssetNotInUniverse,
        "filter reason"
    );

    MarketStateStore store;
    if (decision.passed()) {
        const auto applied = store.apply(event);
        expect_true(applied.ok(), "paired event should not be applied");
    }
    expect_equal(store.exists(kPairedAsset), false, "paired store exists");
}

void TargetAssetDeltaBeforeSnapshotIsNotFiltered() {
    const auto event =
        state_event(ws_event(NormalizedEventType::Delta, kActiveAsset));
    const auto active_universe = universe();
    const MarketStateEventFilter filter(active_universe);
    const auto decision = filter.filter(event);
    expect_true(decision.passed(), "target delta filter pass");

    MarketStateStore store;
    const auto applied = store.apply(event);
    expect_equal(
        applied.code,
        StateApplyCode::DeltaBeforeSnapshot,
        "state error code"
    );
    expect_equal(applied.ok(), false, "target delta before snapshot ok");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"MarketStateEventFilter_PassesActiveAssetSnapshot",
         &MarketStateEventFilter_PassesActiveAssetSnapshot},
        {"MarketStateEventFilter_PassesActiveAssetDelta",
         &MarketStateEventFilter_PassesActiveAssetDelta},
        {"MarketStateEventFilter_FiltersPairedAssetDelta",
         &MarketStateEventFilter_FiltersPairedAssetDelta},
        {"MarketStateEventFilter_FiltersExternalAsset",
         &MarketStateEventFilter_FiltersExternalAsset},
        {"MarketStateEventFilter_PassesHeartbeat",
         &MarketStateEventFilter_PassesHeartbeat},
        {"MarketStateEventFilter_PassesLifecycleForActiveMarket",
         &MarketStateEventFilter_PassesLifecycleForActiveMarket},
        {"MarketStateEventFilter_FiltersLifecycleForInactiveMarket",
         &MarketStateEventFilter_FiltersLifecycleForInactiveMarket},
        {"PairedAssetDeltaIsFilteredBeforeStateStore",
         &PairedAssetDeltaIsFilteredBeforeStateStore},
        {"TargetAssetDeltaBeforeSnapshotIsNotFiltered",
         &TargetAssetDeltaBeforeSnapshotIsNotFiltered}
    };
    return test_map;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: market_state_event_filter_tests <test_name>\n";
        return 2;
    }

    const auto it = tests().find(argv[1]);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << argv[1] << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
