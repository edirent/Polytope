#include "chain_confirm/ClassifiedFillRecord.h"
#include "decode/public/NormalizedEvent.h"
#include "decode/public/NormalizedEventBatch.h"
#include "state/MarketStateView.h"
#include "state/StateTypes.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/MarketStateStore.h"

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
using trading_engine::state::AggressorSide;
using trading_engine::state::BookQuality;
using trading_engine::state::MarketStateStore;
using trading_engine::state::MarketStateView;
using trading_engine::state::StateQueryError;
using trading_engine::state::from_classified_fill;
using trading_engine::state::from_normalized_batch;

constexpr const char* kEntityId = "asset-1";
constexpr const char* kMarketId = "market-1";

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

NormalizedEvent snapshot_event(
    std::uint64_t packet_id = 10,
    std::vector<BookLevel> bids = {{0.50, 100.0}, {0.49, 200.0}},
    std::vector<BookLevel> asks = {{0.54, 150.0}, {0.55, 250.0}}
) {
    NormalizedEvent event;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = packet_id * 1000;
    event.event_type = NormalizedEventType::Snapshot;
    event.entity_id = kEntityId;
    event.asset_id = kEntityId;
    event.market_id = kMarketId;
    event.raw_type = "book";
    event.bids = std::move(bids);
    event.asks = std::move(asks);
    event.tick_size = 0.01;
    return event;
}

NormalizedEvent delta_event(
    std::uint64_t packet_id,
    std::vector<PriceLevelChange> changes
) {
    NormalizedEvent event;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = packet_id * 1000;
    event.event_type = NormalizedEventType::Delta;
    event.entity_id = kEntityId;
    event.asset_id = kEntityId;
    event.market_id = kMarketId;
    event.raw_type = "price_change";
    event.changes = std::move(changes);
    return event;
}

NormalizedEvent lifecycle_event(
    std::string raw_type,
    std::uint64_t packet_id = 12
) {
    NormalizedEvent event;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = packet_id * 1000;
    event.event_type = NormalizedEventType::LifecycleEvent;
    event.entity_id = kEntityId;
    event.asset_id = kEntityId;
    event.market_id = kMarketId;
    event.raw_type = std::move(raw_type);
    if (event.raw_type == "market_resolved") {
        event.winning_asset_id = kEntityId;
    }
    return event;
}

ClassifiedFillRecord chain_fill() {
    ClassifiedFillRecord fill;
    fill.fill_id = "0xtx:1";
    fill.order_hash = "0xorder";
    fill.market_id = kMarketId;
    fill.asset_id = kEntityId;
    fill.price_tick = 500000;
    fill.size_lots = 1000;
    fill.direction = ConfirmedDirection::BuyAggressor;
    fill.mapping_status = FillMappingStatus::Mapped;
    fill.classification = FillClassification::ChainConfirmed;
    fill.block_number = 100;
    fill.tx_hash = "0xtx";
    fill.log_index = 1;
    fill.chain_seen_monotonic_ns = 12'000;
    fill.source_sequence = 100;
    return fill;
}

void apply_normalized(MarketStateStore& store, const NormalizedEvent& event) {
    NormalizedEventBatch batch;
    expect_true(batch.push_back(event), "batch push");
    const auto events = from_normalized_batch(batch);
    expect_equal(events.size(), 1ULL, "market event count");
    const auto result = store.apply(events.front());
    if (!result.ok()) {
        fail("state apply failed: " + result.message);
    }
}

void apply_snapshot(MarketStateStore& store) {
    apply_normalized(store, snapshot_event());
}

void MarketStateView_EmptyStoreReturnsMissing() {
    MarketStateStore store;
    MarketStateView view(store);

    expect_false(view.exists(kEntityId), "exists");

    const auto bid = view.get_best_bid(kEntityId);
    expect_false(bid.ok, "best bid ok");
    expect_equal(
        bid.error,
        StateQueryError::MissingEntity,
        "best bid missing entity"
    );

    const auto snapshot = view.get_snapshot(kEntityId);
    expect_false(snapshot.ok, "snapshot ok");
    expect_equal(
        snapshot.error,
        StateQueryError::MissingEntity,
        "snapshot missing entity"
    );
}

void MarketStateView_SnapshotReturnsBestBidAsk() {
    MarketStateStore store;
    apply_snapshot(store);
    MarketStateView view(store);

    const auto bbo = view.get_bbo(kEntityId);
    expect_true(bbo.ok, "bbo ok");
    expect_equal(bbo.value.bid.price_tick, 50LL, "bid tick");
    expect_equal(bbo.value.ask.price_tick, 54LL, "ask tick");

    const auto snapshot = view.get_snapshot(kEntityId);
    expect_true(snapshot.ok, "snapshot ok");
    expect_true(snapshot.value.has_bid, "snapshot has bid");
    expect_true(snapshot.value.has_ask, "snapshot has ask");
    expect_equal(snapshot.value.bid_count, 2U, "bid_count");
    expect_equal(snapshot.value.ask_count, 2U, "ask_count");
    expect_equal(snapshot.value.bids[0].price_tick, 50LL, "bid order");
    expect_equal(snapshot.value.asks[0].price_tick, 54LL, "ask order");
}

void MarketStateView_DeltaUpdatesBestBidAsk() {
    MarketStateStore store;
    apply_snapshot(store);
    apply_normalized(
        store,
        delta_event(11, {{NormalizedSide::Bid, 0.52, 10.0}})
    );

    MarketStateView view(store);
    const auto bid = view.get_best_bid(kEntityId);
    expect_true(bid.ok, "best bid ok");
    expect_equal(bid.value.price_tick, 52LL, "updated bid tick");
    expect_equal(bid.version, 11ULL, "version");
}

void MarketStateView_MidAndSpreadComputedCorrectly() {
    MarketStateStore store;
    apply_snapshot(store);
    MarketStateView view(store);

    const auto mid = view.get_mid_tick(kEntityId);
    const auto spread = view.get_spread_tick(kEntityId);

    expect_true(mid.ok, "mid ok");
    expect_true(spread.ok, "spread ok");
    expect_equal(mid.value, 52LL, "mid tick");
    expect_equal(spread.value, 4LL, "spread tick");
}

void MarketStateView_MissingBidOrAskReturnsError() {
    MarketStateStore store;
    apply_normalized(store, snapshot_event(10, {}, {{0.54, 100.0}}));

    MarketStateView view(store);
    const auto bid = view.get_best_bid(kEntityId);
    const auto bbo = view.get_bbo(kEntityId);

    expect_false(bid.ok, "bid ok");
    expect_equal(bid.error, StateQueryError::MissingBid, "missing bid");
    expect_false(bbo.ok, "bbo ok");
    expect_equal(bbo.error, StateQueryError::MissingBid, "bbo missing bid");
}

void MarketStateView_RecoveringEntityReturnsNoExecutablePrice() {
    MarketStateStore store;
    NormalizedEventBatch batch;
    expect_true(
        batch.push_back(delta_event(
            1,
            {{NormalizedSide::Bid, 0.50, 1.0}}
        )),
        "batch push"
    );
    const auto events = from_normalized_batch(batch);
    store.apply(events.front());

    MarketStateView view(store);
    const auto bid = view.get_best_bid(kEntityId);

    expect_false(bid.ok, "bid ok");
    expect_equal(bid.error, StateQueryError::Recovering, "recovering");
}

void MarketStateView_ClosedEntityReturnsNoExecutablePrice() {
    MarketStateStore store;
    apply_snapshot(store);
    apply_normalized(store, lifecycle_event("market_closed"));

    MarketStateView view(store);
    const auto bbo = view.get_bbo(kEntityId);

    expect_false(bbo.ok, "bbo ok");
    expect_equal(bbo.error, StateQueryError::Closed, "closed");
}

void MarketStateView_ResolvedEntityReturnsNoExecutablePrice() {
    MarketStateStore store;
    apply_snapshot(store);
    apply_normalized(store, lifecycle_event("market_resolved"));

    MarketStateView view(store);
    const auto bbo = view.get_bbo(kEntityId);

    expect_false(bbo.ok, "bbo ok");
    expect_equal(bbo.error, StateQueryError::Resolved, "resolved");
}

void MarketStateView_CrossedBookReturnsError() {
    MarketStateStore store;
    apply_normalized(
        store,
        snapshot_event(10, {{0.60, 100.0}}, {{0.50, 100.0}})
    );

    MarketStateView view(store);
    const auto bbo = view.get_bbo(kEntityId);

    expect_false(bbo.ok, "bbo ok");
    expect_equal(bbo.error, StateQueryError::CrossedBook, "crossed");
}

void MarketStateView_SnapshotCopyHasStableVersion() {
    MarketStateStore store;
    apply_snapshot(store);
    MarketStateView view(store);

    const auto before = view.get_snapshot(kEntityId);
    apply_normalized(
        store,
        delta_event(11, {{NormalizedSide::Bid, 0.52, 10.0}})
    );
    const auto after = view.get_snapshot(kEntityId);

    expect_true(before.ok, "before snapshot ok");
    expect_true(after.ok, "after snapshot ok");
    expect_equal(before.version, 10ULL, "before version");
    expect_equal(before.value.best_bid_tick, 50LL, "before copy bid");
    expect_equal(after.version, 11ULL, "after version");
    expect_equal(after.value.best_bid_tick, 52LL, "after copy bid");
}

void MarketStateView_StateHashStable() {
    MarketStateStore store;
    apply_snapshot(store);
    MarketStateView view(store);

    const auto snapshot = view.get_snapshot(kEntityId);
    expect_true(snapshot.ok, "snapshot ok");
    expect_equal(
        view.state_hash(kEntityId),
        snapshot.value.state_hash,
        "entity hash"
    );
    expect_equal(view.global_hash(), store.global_hash(), "global hash");
    expect_equal(
        view.state_hash(kEntityId),
        view.state_hash(kEntityId),
        "stable entity hash"
    );
}

void MarketStateView_ReadsPublishedSnapshot() {
    MarketStateStore store;
    apply_snapshot(store);
    MarketStateView view(store);

    const auto snapshot = view.get_snapshot(kEntityId);

    expect_true(snapshot.ok, "snapshot ok");
    expect_equal(snapshot.value.entity_id, std::string{kEntityId}, "asset");
    expect_equal(snapshot.value.best_bid_tick, 50LL, "bid");
}

void MarketStateView_DoesNotReadMutableEntityStore() {
    MarketStateStore store;
    apply_snapshot(store);
    MarketStateView view(store);

    const auto before = view.get_snapshot(kEntityId);
    apply_normalized(
        store,
        delta_event(11, {{NormalizedSide::Bid, 0.52, 10.0}})
    );
    const auto after = view.get_snapshot(kEntityId);

    expect_true(before.ok, "before ok");
    expect_true(after.ok, "after ok");
    expect_equal(before.value.best_bid_tick, 50LL, "before copy stable");
    expect_equal(after.value.best_bid_tick, 52LL, "after published update");
}

void MarketStateView_ReturnsBookAndChainFields() {
    MarketStateStore store;
    apply_snapshot(store);
    store.apply(from_classified_fill(chain_fill()));
    MarketStateView view(store);

    const auto snapshot = view.get_snapshot(kEntityId);
    expect_true(snapshot.ok, "snapshot ok");
    expect_equal(snapshot.value.best_bid_tick, 50LL, "book bid");
    expect_true(snapshot.value.has_confirmed_trade, "has chain trade");
    expect_equal(snapshot.value.last_trade_price_tick, 500000LL, "trade price");
    expect_equal(snapshot.value.last_trade_size_lots, 1000LL, "trade size");
    expect_equal(
        snapshot.value.last_taker_side,
        AggressorSide::Buy,
        "aggressor"
    );
    expect_equal(
        snapshot.value.confirmed_buy_lots_2s,
        1000LL,
        "buy lots 2s"
    );
    expect_equal(snapshot.value.quality, BookQuality::Good, "quality");
    expect_true(snapshot.value.usable_for_depth, "usable depth");
    expect_true(snapshot.value.usable_for_signal, "usable signal");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"MarketStateView_EmptyStoreReturnsMissing",
         &MarketStateView_EmptyStoreReturnsMissing},
        {"MarketStateView_SnapshotReturnsBestBidAsk",
         &MarketStateView_SnapshotReturnsBestBidAsk},
        {"MarketStateView_DeltaUpdatesBestBidAsk",
         &MarketStateView_DeltaUpdatesBestBidAsk},
        {"MarketStateView_MidAndSpreadComputedCorrectly",
         &MarketStateView_MidAndSpreadComputedCorrectly},
        {"MarketStateView_MissingBidOrAskReturnsError",
         &MarketStateView_MissingBidOrAskReturnsError},
        {"MarketStateView_RecoveringEntityReturnsNoExecutablePrice",
         &MarketStateView_RecoveringEntityReturnsNoExecutablePrice},
        {"MarketStateView_ClosedEntityReturnsNoExecutablePrice",
         &MarketStateView_ClosedEntityReturnsNoExecutablePrice},
        {"MarketStateView_ResolvedEntityReturnsNoExecutablePrice",
         &MarketStateView_ResolvedEntityReturnsNoExecutablePrice},
        {"MarketStateView_CrossedBookReturnsError",
         &MarketStateView_CrossedBookReturnsError},
        {"MarketStateView_SnapshotCopyHasStableVersion",
         &MarketStateView_SnapshotCopyHasStableVersion},
        {"MarketStateView_StateHashStable",
         &MarketStateView_StateHashStable},
        {"MarketStateView_ReadsPublishedSnapshot",
         &MarketStateView_ReadsPublishedSnapshot},
        {"MarketStateView_DoesNotReadMutableEntityStore",
         &MarketStateView_DoesNotReadMutableEntityStore},
        {"MarketStateView_ReturnsBookAndChainFields",
         &MarketStateView_ReturnsBookAndChainFields}
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
