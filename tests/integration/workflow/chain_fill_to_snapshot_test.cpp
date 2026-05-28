#include "chain_confirm/ClassifiedFillRecord.h"
#include "decode/public/NormalizedEventBatch.h"
#include "state/MarketStateView.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/MarketStateStore.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
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
using trading_engine::state::MarketStateStore;
using trading_engine::state::MarketStateView;
using trading_engine::state::from_classified_fill;
using trading_engine::state::from_normalized_batch;

constexpr const char* kMarketId = "workflow-market";
constexpr const char* kAssetId = "workflow-asset";

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
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

NormalizedEvent seed_book_snapshot() {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Snapshot;
    event.packet_id = 1;
    event.recv_monotonic_ns = 1'000'000'000ULL;
    event.market_id = kMarketId;
    event.asset_id = kAssetId;
    event.entity_id = kAssetId;
    event.raw_type = "book";
    event.bids = std::vector<BookLevel>{{0.49, 100.0}};
    event.asks = std::vector<BookLevel>{{0.51, 100.0}};
    return event;
}

void apply_seed_book(MarketStateStore* store) {
    NormalizedEventBatch batch;
    expect_true(batch.push_back(seed_book_snapshot()), "seed batch push");
    for (const auto& event : from_normalized_batch(batch)) {
        const auto result = store->apply(event);
        expect_true(result.ok(), "seed book apply");
    }
}

ClassifiedFillRecord fill(
    std::string fill_id,
    ConfirmedDirection direction,
    FillClassification classification,
    FillMappingStatus mapping_status,
    bool removed,
    std::int64_t size_lots
) {
    ClassifiedFillRecord out;
    out.fill_id = std::move(fill_id);
    out.order_hash = "0xworkflow-order";
    out.market_id = kMarketId;
    out.asset_id = kAssetId;
    out.price_tick = 500000;
    out.size_lots = size_lots;
    out.direction = direction;
    out.mapping_status = mapping_status;
    out.classification = classification;
    out.block_number = 100;
    out.tx_hash = "0xworkflow";
    out.log_index = 1;
    out.chain_seen_monotonic_ns = 1'001'000'000ULL;
    out.source_sequence = 100;
    out.removed = removed;
    return out;
}

void apply_chain_fill(
    MarketStateStore* store,
    const ClassifiedFillRecord& classified
) {
    const auto event = from_classified_fill(classified);
    const auto result = store->apply(event);
    expect_true(result.ok(), "chain fill apply");
}

void ChainFillToSnapshot_ConfirmedAndUnsafeFillsAreSeparated() {
    MarketStateStore store;
    MarketStateView view(store);
    apply_seed_book(&store);

    const auto legacy_book_hash = store.global_hash();

    apply_chain_fill(
        &store,
        fill(
            "buy-fill",
            ConfirmedDirection::BuyAggressor,
            FillClassification::ChainConfirmed,
            FillMappingStatus::Mapped,
            false,
            1000
        )
    );
    apply_chain_fill(
        &store,
        fill(
            "sell-fill",
            ConfirmedDirection::SellAggressor,
            FillClassification::ChainConfirmed,
            FillMappingStatus::Mapped,
            false,
            700
        )
    );
    apply_chain_fill(
        &store,
        fill(
            "ambiguous-fill",
            ConfirmedDirection::Unknown,
            FillClassification::AmbiguousFill,
            FillMappingStatus::AmbiguousFill,
            false,
            333
        )
    );
    apply_chain_fill(
        &store,
        fill(
            "buy-fill",
            ConfirmedDirection::BuyAggressor,
            FillClassification::ChainRemoved,
            FillMappingStatus::Mapped,
            true,
            1000
        )
    );

    expect_equal(store.global_hash(), legacy_book_hash, "legacy book hash");

    const auto snapshot = view.get_snapshot(kAssetId);
    expect_true(snapshot.ok, "snapshot ok");
    expect_true(snapshot.value.has_confirmed_trade, "has chain state");
    expect_equal(
        snapshot.value.last_taker_side,
        AggressorSide::Sell,
        "last taker side"
    );
    expect_equal(snapshot.value.confirmed_buy_lots_10s, 0LL, "buy lots");
    expect_equal(snapshot.value.confirmed_sell_lots_10s, 700LL, "sell lots");
    expect_equal(snapshot.value.bid_count, 1U, "bid count unchanged");
    expect_equal(snapshot.value.ask_count, 1U, "ask count unchanged");
}

}  // namespace

int main() {
    try {
        ChainFillToSnapshot_ConfirmedAndUnsafeFillsAreSeparated();
        std::cout
            << "ChainFillToSnapshot_ConfirmedAndUnsafeFillsAreSeparated passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "ChainFillToSnapshot_ConfirmedAndUnsafeFillsAreSeparated failed: "
            << error.what() << '\n';
        return 1;
    }
}
