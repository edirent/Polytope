#include "chain_confirm/ClassifiedFillRecord.h"
#include "decode/public/NormalizedEventBatch.h"
#include "state/MarketStateView.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/MarketStateStore.h"
#include "state/quality/BookQualityState.h"

#include <cstdint>
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
using trading_engine::decode::NormalizedSide;
using trading_engine::decode::PriceLevelChange;
using trading_engine::state::BookQuality;
using trading_engine::state::MarketStateStore;
using trading_engine::state::MarketStateView;
using trading_engine::state::StateApplyCode;
using trading_engine::state::from_classified_fill;
using trading_engine::state::from_normalized_batch;

constexpr const char* kMarketA = "market-a";
constexpr const char* kAssetA = "asset-a";
constexpr const char* kMarketB = "market-b";
constexpr const char* kAssetB = "asset-b";
constexpr const char* kMarketC = "market-c";
constexpr const char* kAssetC = "asset-c";

struct Report {
    bool asset_a_snapshot_ok{false};
    bool asset_a_chain_ok{false};
    bool asset_a_book_hash_unchanged{false};
    std::int64_t asset_a_buy_lots{0};
    std::int64_t asset_a_sell_lots{0};

    bool asset_b_snapshot_ok{false};
    bool asset_b_ambiguous_not_counted{false};
    std::int64_t asset_b_buy_lots{0};
    std::int64_t asset_b_sell_lots{0};

    bool asset_c_snapshot_ok{false};
    bool asset_c_recovering{false};

    bool asset_isolation_ok{false};
    std::uint64_t asset_a_hash{0};
    std::uint64_t asset_b_hash{0};
    std::uint64_t asset_c_hash{0};
    std::uint64_t global_book_hash{0};
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

NormalizedEvent snapshot_event(
    std::string market_id,
    std::string asset_id,
    std::uint64_t packet_id,
    double bid,
    double ask
) {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Snapshot;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = packet_id * 1'000'000'000ULL;
    event.market_id = std::move(market_id);
    event.asset_id = asset_id;
    event.entity_id = std::move(asset_id);
    event.raw_type = "book";
    event.bids = std::vector<BookLevel>{{bid, 100.0}};
    event.asks = std::vector<BookLevel>{{ask, 100.0}};
    return event;
}

NormalizedEvent delta_event(
    std::string market_id,
    std::string asset_id,
    std::uint64_t packet_id,
    double price,
    double size
) {
    NormalizedEvent event;
    event.event_type = NormalizedEventType::Delta;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = packet_id * 1'000'000'000ULL;
    event.market_id = std::move(market_id);
    event.asset_id = asset_id;
    event.entity_id = std::move(asset_id);
    event.raw_type = "price_change";
    event.changes = std::vector<PriceLevelChange>{
        {NormalizedSide::Bid, price, size}
    };
    return event;
}

ClassifiedFillRecord fill(
    std::string fill_id,
    std::string market_id,
    std::string asset_id,
    ConfirmedDirection direction,
    FillClassification classification,
    FillMappingStatus mapping_status,
    bool removed,
    std::int64_t size_lots,
    std::uint64_t sequence
) {
    ClassifiedFillRecord out;
    out.fill_id = std::move(fill_id);
    out.order_hash = "0xorder";
    out.market_id = std::move(market_id);
    out.asset_id = std::move(asset_id);
    out.price_tick = 500000;
    out.size_lots = size_lots;
    out.direction = direction;
    out.mapping_status = mapping_status;
    out.classification = classification;
    out.block_number = 1000 + sequence;
    out.tx_hash = "0xsynthetic" + std::to_string(sequence);
    out.log_index = static_cast<std::uint32_t>(sequence);
    out.chain_seen_monotonic_ns = 10'000'000'000ULL + sequence;
    out.source_sequence = sequence;
    out.removed = removed;
    return out;
}

StateApplyCode apply_normalized(
    MarketStateStore* store,
    const NormalizedEvent& event
) {
    NormalizedEventBatch batch;
    if (!batch.push_back(event)) {
        fail("failed to build normalized batch");
    }

    StateApplyCode code = StateApplyCode::Noop;
    for (const auto& state_event : from_normalized_batch(batch)) {
        const auto result = store->apply(state_event);
        code = result.code;
    }
    return code;
}

void apply_fill(MarketStateStore* store, const ClassifiedFillRecord& record) {
    const auto result = store->apply(from_classified_fill(record));
    if (!result.ok()) {
        fail("chain fill apply failed: " + result.message);
    }
}

Report run_workflow() {
    MarketStateStore store;
    MarketStateView view(store);

    apply_normalized(&store, snapshot_event(kMarketA, kAssetA, 1, 0.49, 0.51));
    apply_normalized(&store, snapshot_event(kMarketB, kAssetB, 2, 0.40, 0.60));

    const auto asset_a_hash_before_chain = view.state_hash(kAssetA);
    const auto asset_b_hash_before_chain = view.state_hash(kAssetB);

    apply_fill(
        &store,
        fill(
            "asset-a-buy",
            kMarketA,
            kAssetA,
            ConfirmedDirection::BuyAggressor,
            FillClassification::ChainConfirmed,
            FillMappingStatus::Mapped,
            false,
            1000,
            1
        )
    );
    apply_fill(
        &store,
        fill(
            "asset-a-sell",
            kMarketA,
            kAssetA,
            ConfirmedDirection::SellAggressor,
            FillClassification::ChainConfirmed,
            FillMappingStatus::Mapped,
            false,
            700,
            2
        )
    );
    apply_fill(
        &store,
        fill(
            "asset-b-ambiguous",
            kMarketB,
            kAssetB,
            ConfirmedDirection::Unknown,
            FillClassification::AmbiguousFill,
            FillMappingStatus::AmbiguousFill,
            false,
            333,
            3
        )
    );

    const StateApplyCode asset_c_code = apply_normalized(
        &store,
        delta_event(kMarketC, kAssetC, 3, 0.44, 50.0)
    );

    const auto asset_a = view.get_snapshot(kAssetA);
    const auto asset_b = view.get_snapshot(kAssetB);
    const auto asset_c = view.get_snapshot(kAssetC);

    if (!asset_a.ok || !asset_b.ok || !asset_c.ok) {
        fail("expected all asset snapshots to be queryable");
    }

    Report report;
    report.asset_a_snapshot_ok = asset_a.ok;
    report.asset_a_chain_ok =
        asset_a.value.has_confirmed_trade &&
        asset_a.value.confirmed_buy_lots_10s == 1000 &&
        asset_a.value.confirmed_sell_lots_10s == 700;
    report.asset_a_book_hash_unchanged =
        asset_a.value.state_hash == asset_a_hash_before_chain;
    report.asset_a_buy_lots = asset_a.value.confirmed_buy_lots_10s;
    report.asset_a_sell_lots = asset_a.value.confirmed_sell_lots_10s;

    report.asset_b_snapshot_ok = asset_b.ok;
    report.asset_b_ambiguous_not_counted =
        asset_b.value.confirmed_buy_lots_10s == 0 &&
        asset_b.value.confirmed_sell_lots_10s == 0;
    report.asset_b_buy_lots = asset_b.value.confirmed_buy_lots_10s;
    report.asset_b_sell_lots = asset_b.value.confirmed_sell_lots_10s;

    report.asset_c_snapshot_ok = asset_c.ok;
    report.asset_c_recovering =
        asset_c_code == StateApplyCode::DeltaBeforeSnapshot &&
        asset_c.value.recovering &&
        asset_c.value.quality == BookQuality::Recovering;

    report.asset_a_hash = asset_a.value.state_hash;
    report.asset_b_hash = asset_b.value.state_hash;
    report.asset_c_hash = asset_c.value.state_hash;
    report.global_book_hash = store.global_hash();
    report.asset_isolation_ok =
        report.asset_a_book_hash_unchanged &&
        asset_b.value.state_hash == asset_b_hash_before_chain &&
        asset_a.value.entity_id == kAssetA &&
        asset_b.value.entity_id == kAssetB &&
        asset_c.value.entity_id == kAssetC;

    return report;
}

void validate(const Report& report) {
    if (!report.asset_a_snapshot_ok ||
        !report.asset_a_chain_ok ||
        !report.asset_a_book_hash_unchanged ||
        !report.asset_b_snapshot_ok ||
        !report.asset_b_ambiguous_not_counted ||
        !report.asset_c_snapshot_ok ||
        !report.asset_c_recovering ||
        !report.asset_isolation_ok) {
        fail("multi-asset workflow validation failed");
    }
}

void print_bool(const char* name, bool value) {
    std::cout << "  " << name << ": " << (value ? "true" : "false") << '\n';
}

void print_report(const Report& report) {
    std::cout << "multi_asset_synthetic_workflow:\n";
    std::cout << "asset_a:\n";
    print_bool("snapshot_ok", report.asset_a_snapshot_ok);
    print_bool("chain_state_ok", report.asset_a_chain_ok);
    print_bool("book_hash_unchanged", report.asset_a_book_hash_unchanged);
    std::cout << "  confirmed_buy_lots_10s: "
              << report.asset_a_buy_lots << '\n';
    std::cout << "  confirmed_sell_lots_10s: "
              << report.asset_a_sell_lots << '\n';

    std::cout << "asset_b:\n";
    print_bool("snapshot_ok", report.asset_b_snapshot_ok);
    print_bool(
        "ambiguous_not_counted",
        report.asset_b_ambiguous_not_counted
    );
    std::cout << "  confirmed_buy_lots_10s: "
              << report.asset_b_buy_lots << '\n';
    std::cout << "  confirmed_sell_lots_10s: "
              << report.asset_b_sell_lots << '\n';

    std::cout << "asset_c:\n";
    print_bool("snapshot_ok", report.asset_c_snapshot_ok);
    print_bool("recovering", report.asset_c_recovering);

    std::cout << "isolation:\n";
    print_bool("asset_isolation_ok", report.asset_isolation_ok);
    std::cout << "  asset_a_hash: " << report.asset_a_hash << '\n';
    std::cout << "  asset_b_hash: " << report.asset_b_hash << '\n';
    std::cout << "  asset_c_hash: " << report.asset_c_hash << '\n';
    std::cout << "  global_book_hash: " << report.global_book_hash << '\n';
}

}  // namespace

int main() {
    try {
        const Report report = run_workflow();
        validate(report);
        print_report(report);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "multi_asset_synthetic_workflow failed: "
                  << error.what() << '\n';
        return 1;
    }
}
