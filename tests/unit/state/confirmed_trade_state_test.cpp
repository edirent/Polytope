#include "chain_confirm/ClassifiedFillRecord.h"
#include "decode/public/NormalizedEventBatch.h"
#include "state/EntityStateStore.h"
#include "state/chain/ChainStateWriter.h"
#include "state/chain/ConfirmedTradeState.h"
#include "state/core/MarketStateEventAdapter.h"

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
using trading_engine::state::AggressorSide;
using trading_engine::state::ChainApplyCode;
using trading_engine::state::ChainStateWriter;
using trading_engine::state::EntityStateStore;
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

ClassifiedFillRecord fill(
    std::string fill_id,
    ConfirmedDirection direction,
    std::int64_t size_lots = 1000,
    std::uint64_t chain_seen_ns = 1'000'000'000ULL,
    FillClassification classification = FillClassification::ChainConfirmed,
    FillMappingStatus mapping_status = FillMappingStatus::Mapped,
    bool removed = false
) {
    ClassifiedFillRecord out;
    out.fill_id = std::move(fill_id);
    out.order_hash = "0xorder";
    out.market_id = kMarketId;
    out.asset_id = kAssetId;
    out.price_tick = 500000;
    out.size_lots = size_lots;
    out.direction = direction;
    out.mapping_status = mapping_status;
    out.classification = classification;
    out.block_number = 100;
    out.tx_hash = "0xtx";
    out.log_index = 1;
    out.chain_seen_monotonic_ns = chain_seen_ns;
    out.source_sequence = 100;
    out.removed = removed;
    return out;
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

void ChainConfirmedFill_BuyAggressorUpdatesBuyWindow() {
    ChainStateWriter writer;

    const auto result = writer.apply(from_classified_fill(fill(
        "fill-buy",
        ConfirmedDirection::BuyAggressor,
        1200
    )));

    expect_equal(result.code, ChainApplyCode::Applied, "apply code");
    const auto* state = writer.get(kAssetId);
    expect_true(state != nullptr, "state exists");
    expect_true(state->has_last_trade, "has last trade");
    expect_equal(state->last_trade_price_tick, 500000LL, "price");
    expect_equal(state->last_trade_size_lots, 1200LL, "size");
    expect_equal(state->last_taker_side, AggressorSide::Buy, "side");
    expect_equal(state->confirmed_buy_lots_2s, 1200LL, "buy 2s");
    expect_equal(state->confirmed_sell_lots_2s, 0LL, "sell 2s");
    expect_equal(state->confirmed_buy_lots_10s, 1200LL, "buy 10s");
    expect_equal(state->confirmed_sell_lots_10s, 0LL, "sell 10s");
}

void ChainConfirmedFill_SellAggressorUpdatesSellWindow() {
    ChainStateWriter writer;

    const auto result = writer.apply(from_classified_fill(fill(
        "fill-sell",
        ConfirmedDirection::SellAggressor,
        900
    )));

    expect_equal(result.code, ChainApplyCode::Applied, "apply code");
    const auto* state = writer.get(kAssetId);
    expect_true(state != nullptr, "state exists");
    expect_equal(state->last_taker_side, AggressorSide::Sell, "side");
    expect_equal(state->confirmed_buy_lots_2s, 0LL, "buy 2s");
    expect_equal(state->confirmed_sell_lots_2s, 900LL, "sell 2s");
    expect_equal(state->confirmed_buy_lots_10s, 0LL, "buy 10s");
    expect_equal(state->confirmed_sell_lots_10s, 900LL, "sell 10s");
}

void UnknownDirection_DoesNotBecomeBuyOrSell() {
    ChainStateWriter writer;

    const auto result = writer.apply(from_classified_fill(fill(
        "fill-unknown",
        ConfirmedDirection::Unknown,
        700
    )));

    expect_equal(result.code, ChainApplyCode::UnknownDirection, "apply code");
    const auto* state = writer.get(kAssetId);
    expect_true(state != nullptr, "state exists");
    expect_equal(state->unknown_fill_count_recent, 1U, "unknown count");
    expect_equal(state->confirmed_buy_lots_2s, 0LL, "buy 2s");
    expect_equal(state->confirmed_sell_lots_2s, 0LL, "sell 2s");
    expect_equal(state->last_taker_side, AggressorSide::Unknown, "side");
}

void AmbiguousDirection_DoesNotBecomeBuyOrSell() {
    ChainStateWriter writer;

    const auto result = writer.apply(from_classified_fill(fill(
        "fill-ambiguous",
        ConfirmedDirection::BuyAggressor,
        800,
        1'000'000'000ULL,
        FillClassification::AmbiguousFill,
        FillMappingStatus::AmbiguousFill
    )));

    expect_equal(result.code, ChainApplyCode::AmbiguousFill, "apply code");
    const auto* state = writer.get(kAssetId);
    expect_true(state != nullptr, "state exists");
    expect_equal(state->ambiguous_fill_count_recent, 1U, "ambiguous count");
    expect_equal(state->confirmed_buy_lots_2s, 0LL, "buy 2s");
    expect_equal(state->confirmed_sell_lots_2s, 0LL, "sell 2s");
    expect_equal(state->last_taker_side, AggressorSide::Unknown, "side");
}

void RemovedFill_MarksOrRevertsTrade() {
    ChainStateWriter writer;

    writer.apply(from_classified_fill(fill(
        "fill-remove",
        ConfirmedDirection::BuyAggressor,
        1000,
        1'000'000'000ULL
    )));

    const auto result = writer.apply(from_classified_fill(fill(
        "fill-remove",
        ConfirmedDirection::BuyAggressor,
        1000,
        1'500'000'000ULL,
        FillClassification::ChainRemoved,
        FillMappingStatus::Mapped,
        true
    )));

    expect_equal(result.code, ChainApplyCode::RemovedFill, "apply code");
    const auto* state = writer.get(kAssetId);
    expect_true(state != nullptr, "state exists");
    expect_equal(state->removed_fill_count_recent, 1U, "removed count");
    expect_equal(state->confirmed_buy_lots_2s, 0LL, "buy 2s");
    expect_equal(state->confirmed_sell_lots_2s, 0LL, "sell 2s");
    expect_equal(state->confirmed_buy_lots_10s, 0LL, "buy 10s");
    expect_equal(state->confirmed_sell_lots_10s, 0LL, "sell 10s");
}

void ChainConfirmedFill_DoesNotMutateOrderBook() {
    EntityStateStore store;
    NormalizedEventBatch batch;
    expect_true(batch.push_back(snapshot_event()), "batch push");

    const auto ws_events = from_normalized_batch(batch);
    expect_equal(ws_events.size(), 1ULL, "ws event count");
    const auto apply_result = store.apply(ws_events.front().ws_event);
    expect_true(apply_result.ok(), "snapshot apply");

    const auto before_hash = store.global_hash();
    const auto before_snapshots = store.snapshots_applied();
    const auto before_events = store.events_seen();

    ChainStateWriter writer;
    writer.apply(from_classified_fill(fill(
        "fill-book-isolation",
        ConfirmedDirection::BuyAggressor,
        500
    )));

    expect_equal(store.global_hash(), before_hash, "global hash");
    expect_equal(store.snapshots_applied(), before_snapshots, "snapshots");
    expect_equal(store.events_seen(), before_events, "events seen");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"ChainConfirmedFill_BuyAggressorUpdatesBuyWindow",
         &ChainConfirmedFill_BuyAggressorUpdatesBuyWindow},
        {"ChainConfirmedFill_SellAggressorUpdatesSellWindow",
         &ChainConfirmedFill_SellAggressorUpdatesSellWindow},
        {"UnknownDirection_DoesNotBecomeBuyOrSell",
         &UnknownDirection_DoesNotBecomeBuyOrSell},
        {"AmbiguousDirection_DoesNotBecomeBuyOrSell",
         &AmbiguousDirection_DoesNotBecomeBuyOrSell},
        {"RemovedFill_MarksOrRevertsTrade",
         &RemovedFill_MarksOrRevertsTrade},
        {"ChainConfirmedFill_DoesNotMutateOrderBook",
         &ChainConfirmedFill_DoesNotMutateOrderBook}
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
