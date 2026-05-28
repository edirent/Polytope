#include "chain_confirm/ClassifiedFillRecord.h"
#include "decode/public/NormalizedEventBatch.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/quality/BookQualityState.h"
#include "state/shard/LOBShard.h"
#include "state/shard/ShardRouter.h"

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
using trading_engine::state::LOBShard;
using trading_engine::state::ShardRouter;
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

trading_engine::state::MarketStateEvent market_event_from(
    const NormalizedEvent& event
) {
    NormalizedEventBatch batch;
    expect_true(batch.push_back(event), "batch push");
    const auto events = from_normalized_batch(batch);
    expect_equal(events.size(), 1ULL, "event count");
    return events.front();
}

ClassifiedFillRecord fill(
    std::string fill_id,
    ConfirmedDirection direction = ConfirmedDirection::BuyAggressor,
    bool removed = false
) {
    ClassifiedFillRecord out;
    out.fill_id = std::move(fill_id);
    out.order_hash = "0xorder";
    out.market_id = kMarketId;
    out.asset_id = kAssetId;
    out.price_tick = 500000;
    out.size_lots = 1000;
    out.direction = direction;
    out.mapping_status = FillMappingStatus::Mapped;
    out.classification = removed
        ? FillClassification::ChainRemoved
        : FillClassification::ChainConfirmed;
    out.block_number = 100;
    out.tx_hash = "0xtx";
    out.log_index = 1;
    out.chain_seen_monotonic_ns = 3'000'000'000ULL;
    out.source_sequence = 100;
    out.removed = removed;
    return out;
}

void LOBShard_AppliesWsSnapshotAndPublishesSnapshot() {
    LOBShard shard(0);

    const auto result = shard.apply(market_event_from(snapshot_event()));
    const auto snapshot = shard.snapshot(kAssetId);

    expect_equal(result.code, StateApplyCode::Applied, "apply code");
    expect_true(snapshot.ok, "snapshot ok");
    expect_equal(snapshot.value.entity_id, std::string{kAssetId}, "asset");
    expect_equal(snapshot.value.best_bid_tick, 500000LL, "best bid");
    expect_equal(snapshot.value.best_ask_tick, 540000LL, "best ask");
    expect_equal(snapshot.value.bid_count, 1U, "bid count");
    expect_equal(snapshot.value.ask_count, 1U, "ask count");
}

void LOBShard_AppliesWsDeltaAndUpdatesSnapshot() {
    LOBShard shard(0);
    shard.apply(market_event_from(snapshot_event()));

    const auto result = shard.apply(market_event_from(delta_event()));
    const auto snapshot = shard.snapshot(kAssetId);

    expect_equal(result.code, StateApplyCode::Applied, "apply code");
    expect_true(snapshot.ok, "snapshot ok");
    expect_equal(snapshot.value.best_bid_tick, 520000LL, "updated bid");
    expect_equal(snapshot.value.version, 2ULL, "snapshot version");
}

void LOBShard_AppliesChainFillWithoutChangingBook() {
    LOBShard shard(0);
    shard.apply(market_event_from(snapshot_event()));
    const auto before = shard.snapshot(kAssetId);
    const auto before_hash = shard.book_hash();
    expect_true(before.ok, "before snapshot");

    const auto result = shard.apply(from_classified_fill(fill("fill-1")));
    const auto after = shard.snapshot(kAssetId);
    const auto* chain_state = shard.confirmed_trade_state(kAssetId);

    expect_equal(result.code, StateApplyCode::Noop, "apply code");
    expect_true(result.state_changed, "chain state changed");
    expect_true(after.ok, "after snapshot");
    expect_equal(after.value.state_hash, before.value.state_hash, "state hash");
    expect_equal(shard.book_hash(), before_hash, "book hash");
    expect_equal(after.value.best_bid_tick, before.value.best_bid_tick, "bid");
    expect_true(chain_state != nullptr, "chain state");
    expect_equal(chain_state->confirmed_buy_lots_2s, 1000LL, "buy lots");
    expect_equal(chain_state->last_taker_side, AggressorSide::Buy, "side");
}

void LOBShard_RemovedFillUpdatesChainState() {
    LOBShard shard(0);
    shard.apply(market_event_from(snapshot_event()));
    shard.apply(from_classified_fill(fill("fill-remove")));

    const auto result = shard.apply(
        from_classified_fill(fill("fill-remove", ConfirmedDirection::BuyAggressor, true))
    );

    const auto* chain_state = shard.confirmed_trade_state(kAssetId);
    expect_equal(result.code, StateApplyCode::Noop, "apply code");
    expect_true(chain_state != nullptr, "chain state");
    expect_equal(chain_state->removed_fill_count_recent, 1U, "removed count");
    expect_equal(chain_state->confirmed_buy_lots_2s, 0LL, "buy lots");
    expect_equal(chain_state->confirmed_sell_lots_2s, 0LL, "sell lots");
}

void LOBShard_PublishesQualityState() {
    LOBShard shard(0);
    shard.apply(market_event_from(snapshot_event()));

    const auto& after_book = shard.quality();
    expect_equal(after_book.quality, BookQuality::ChainLagging, "book quality");
    expect_true(after_book.usable_for_depth, "depth usable while chain lagging");

    shard.apply(from_classified_fill(fill("fill-quality")));
    const auto& after_chain = shard.quality();

    expect_equal(after_chain.quality, BookQuality::Good, "quality");
    expect_true(after_chain.usable_for_depth, "depth usable");
    expect_true(after_chain.usable_for_signal, "signal usable");
}

void ShardRouter_ReturnsZeroForNow() {
    ShardRouter router;

    expect_equal(ShardRouter::kNumShards, 1U, "num shards");
    expect_equal(router.shard_for_asset(kAssetId), 0U, "asset shard");
    expect_equal(router.shard_for_asset("other-asset"), 0U, "other shard");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"LOBShard_AppliesWsSnapshotAndPublishesSnapshot",
         &LOBShard_AppliesWsSnapshotAndPublishesSnapshot},
        {"LOBShard_AppliesWsDeltaAndUpdatesSnapshot",
         &LOBShard_AppliesWsDeltaAndUpdatesSnapshot},
        {"LOBShard_AppliesChainFillWithoutChangingBook",
         &LOBShard_AppliesChainFillWithoutChangingBook},
        {"LOBShard_RemovedFillUpdatesChainState",
         &LOBShard_RemovedFillUpdatesChainState},
        {"LOBShard_PublishesQualityState",
         &LOBShard_PublishesQualityState},
        {"ShardRouter_ReturnsZeroForNow", &ShardRouter_ReturnsZeroForNow}
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
