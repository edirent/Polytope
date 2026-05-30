#include "engine/signal/reader/FixtureMarketSnapshotReader.h"
#include "engine/signal/reader/MarketStateViewSnapshotReader.h"

#include "decode/public/NormalizedEvent.h"
#include "decode/public/NormalizedEventBatch.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/MarketStateStore.h"
#include "state/MarketStateView.h"

#include <exception>
#include <filesystem>
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
using trading_engine::oracle::BundleLeg;
using trading_engine::oracle::CandidateBundle;
using trading_engine::oracle::Side;
using trading_engine::signal::FixtureMarketSnapshotReader;
using trading_engine::signal::IntentStatus;
using trading_engine::signal::MarketStateViewSnapshotReader;
using trading_engine::signal::SignalConfig;
using trading_engine::signal::validate_bundle_snapshots;
using trading_engine::state::BookQuality;
using trading_engine::state::MarketStateSnapshot;
using trading_engine::state::MarketStateStore;
using trading_engine::state::MarketStateView;
using trading_engine::state::from_normalized_batch;

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

std::filesystem::path source_path(const std::string& relative) {
    return std::filesystem::path{POLYTOPE_SOURCE_DIR} / relative;
}

CandidateBundle bundle_for(std::initializer_list<const char*> asset_ids) {
    CandidateBundle bundle;
    bundle.bundle_id = 1;
    bundle.guaranteed_payout_tick = 1'000'000;
    for (const auto* asset_id : asset_ids) {
        auto& leg = bundle.legs[bundle.leg_count++];
        leg.market_id = "m1";
        leg.asset_id = asset_id;
        leg.side = Side::Buy;
        leg.quantity_lots = 1;
        leg.max_price_tick = 500000;
    }
    return bundle;
}

FixtureMarketSnapshotReader good_reader() {
    FixtureMarketSnapshotReader reader;
    std::string error;
    expect_true(
        reader.load(
            source_path("tests/fixtures/signal/market_state_snapshots_small.json"),
            &error
        ),
        "load good fixture: " + error
    );
    return reader;
}

FixtureMarketSnapshotReader bad_reader() {
    FixtureMarketSnapshotReader reader;
    std::string error;
    expect_true(
        reader.load(
            source_path("tests/fixtures/signal/market_state_snapshots_bad_state.json"),
            &error
        ),
        "load bad fixture: " + error
    );
    return reader;
}

NormalizedEvent snapshot_event(const std::string& asset_id) {
    NormalizedEvent event;
    event.packet_id = 1;
    event.recv_monotonic_ns = 1000;
    event.event_type = NormalizedEventType::Snapshot;
    event.entity_id = asset_id;
    event.asset_id = asset_id;
    event.market_id = "m1";
    event.raw_type = "book";
    event.bids = std::vector<BookLevel>{{0.49, 100.0}, {0.48, 200.0}};
    event.asks = std::vector<BookLevel>{{0.50, 100.0}, {0.51, 200.0}};
    event.tick_size = 0.01;
    return event;
}

void apply_snapshot(MarketStateStore& store, const std::string& asset_id) {
    NormalizedEventBatch batch;
    expect_true(batch.push_back(snapshot_event(asset_id)), "batch push");
    const auto events = from_normalized_batch(batch);
    expect_equal(events.size(), 1ULL, "event count");
    const auto result = store.apply(events.front());
    if (!result.ok()) {
        fail("state apply failed: " + result.message);
    }
}

void MarketSnapshotReader_ReadsRequiredSnapshots() {
    const auto reader = good_reader();
    const auto result = reader.read_for_bundle(
        bundle_for({"asset_yes", "asset_no"}),
        SignalConfig{}
    );

    expect_true(result.ok, "read ok: " + result.error);
    expect_equal(result.snapshots.size(), 2U, "snapshot count");
    expect_equal(
        result.rejection_status,
        IntentStatus::CandidateOnly,
        "status"
    );
}

void MarketSnapshotReader_ReadsUniqueAssetSnapshots() {
    const auto reader = good_reader();
    const auto result = reader.read_for_bundle(
        bundle_for({"asset_yes", "asset_yes", "asset_no"}),
        SignalConfig{}
    );

    expect_true(result.ok, "read ok: " + result.error);
    expect_equal(result.snapshots.size(), 2U, "unique snapshot count");
}

void MarketSnapshotReader_ReturnsSnapshotVersion() {
    const auto reader = good_reader();
    const auto result = reader.read_for_bundle(
        bundle_for({"asset_yes", "asset_no"}),
        SignalConfig{}
    );

    expect_true(result.ok, "read ok: " + result.error);
    expect_equal(
        result.snapshot_version.min_book_version,
        10ULL,
        "min book version"
    );
    expect_equal(
        result.snapshot_version.max_book_version,
        10ULL,
        "max book version"
    );
    expect_true(
        result.snapshot_version.combined_hash != 0,
        "combined hash"
    );
}

void MarketSnapshotReader_RejectsMissingSnapshot() {
    const auto reader = good_reader();
    const auto result = reader.read_for_bundle(
        bundle_for({"asset_yes", "asset_missing"}),
        SignalConfig{}
    );

    expect_false(result.ok, "read ok");
    expect_equal(
        result.rejection_status,
        IntentStatus::RejectedMissingSnapshot,
        "status"
    );
}

void MarketSnapshotReader_RejectsRecoveringSnapshot() {
    const auto reader = bad_reader();
    const auto result = reader.read_for_bundle(
        bundle_for({"asset_recovering"}),
        SignalConfig{}
    );

    expect_false(result.ok, "read ok");
    expect_equal(
        result.rejection_status,
        IntentStatus::RejectedBadMarketState,
        "status"
    );
}

void MarketSnapshotReader_RejectsCrossedSnapshot() {
    const auto reader = bad_reader();
    const auto result = reader.read_for_bundle(
        bundle_for({"asset_crossed"}),
        SignalConfig{}
    );

    expect_false(result.ok, "read ok");
    expect_equal(
        result.rejection_status,
        IntentStatus::RejectedBadMarketState,
        "status"
    );
}

void MarketSnapshotReader_RejectsNotUsableForDepth() {
    const auto reader = bad_reader();
    const auto result = reader.read_for_bundle(
        bundle_for({"asset_not_depth"}),
        SignalConfig{}
    );

    expect_false(result.ok, "read ok");
    expect_equal(
        result.rejection_status,
        IntentStatus::RejectedBadMarketState,
        "status"
    );
}

void MarketSnapshotReader_RequiresUsableForSignalWhenConfigured() {
    const auto reader = bad_reader();
    SignalConfig config;
    config.require_usable_for_signal = true;
    const auto result = reader.read_for_bundle(
        bundle_for({"asset_signal_false"}),
        config
    );

    expect_false(result.ok, "read ok");
    expect_equal(
        result.rejection_status,
        IntentStatus::RejectedBadMarketState,
        "status"
    );
}

MarketStateSnapshot direct_snapshot(
    const std::string& asset_id,
    std::uint64_t version
) {
    MarketStateSnapshot snapshot;
    snapshot.entity_id = asset_id;
    snapshot.market_id = "m1";
    snapshot.version = version;
    snapshot.last_book_update_ns = 1'000;
    snapshot.live = true;
    snapshot.has_bid = true;
    snapshot.has_ask = true;
    snapshot.best_bid_tick = 490'000;
    snapshot.best_ask_tick = 500'000;
    snapshot.bid_count = 1;
    snapshot.ask_count = 1;
    snapshot.state_hash = version * 100;
    snapshot.quality = BookQuality::Good;
    snapshot.usable_for_depth = true;
    snapshot.usable_for_signal = true;
    return snapshot;
}

void MarketSnapshotReader_UsesConsistencyGuard() {
    SignalConfig config;
    config.max_snapshot_version_skew = 0;

    const std::vector<MarketStateSnapshot> snapshots{
        direct_snapshot("asset_a", 100),
        direct_snapshot("asset_b", 101)
    };

    const auto result = validate_bundle_snapshots(
        bundle_for({"asset_a", "asset_b"}),
        config,
        snapshots,
        1'000
    );

    expect_false(result.ok, "read ok");
    expect_equal(
        result.rejection_status,
        IntentStatus::RejectedBadMarketState,
        "status"
    );
}

void MarketSnapshotReader_MarketStateViewAdapterReadsSnapshot() {
    MarketStateStore store;
    apply_snapshot(store, "asset_yes");
    apply_snapshot(store, "asset_no");
    MarketStateView view(store);
    MarketStateViewSnapshotReader reader(view);

    const auto result = reader.read_for_bundle(
        bundle_for({"asset_yes", "asset_no"}),
        SignalConfig{}
    );

    expect_true(result.ok, "read ok: " + result.error);
    expect_equal(result.snapshots.size(), 2U, "snapshot count");
}

void MarketSnapshotReader_RejectsStaleWhenScanNowExceedsMaxAge() {
    MarketStateStore store;
    apply_snapshot(store, "asset_yes");
    MarketStateView view(store);
    MarketStateViewSnapshotReader reader(view);

    SignalConfig config;
    config.max_lob_age_ns = 500;
    const auto result = reader.read_for_bundle(
        bundle_for({"asset_yes"}),
        config,
        2'000
    );

    expect_false(result.ok, "read ok");
    expect_equal(
        result.rejection_status,
        IntentStatus::RejectedBadMarketState,
        "status"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "MarketSnapshotReader_ReadsRequiredSnapshots",
            &MarketSnapshotReader_ReadsRequiredSnapshots
        },
        {
            "MarketSnapshotReader_ReadsUniqueAssetSnapshots",
            &MarketSnapshotReader_ReadsUniqueAssetSnapshots
        },
        {
            "MarketSnapshotReader_ReturnsSnapshotVersion",
            &MarketSnapshotReader_ReturnsSnapshotVersion
        },
        {
            "MarketSnapshotReader_RejectsMissingSnapshot",
            &MarketSnapshotReader_RejectsMissingSnapshot
        },
        {
            "MarketSnapshotReader_RejectsRecoveringSnapshot",
            &MarketSnapshotReader_RejectsRecoveringSnapshot
        },
        {
            "MarketSnapshotReader_RejectsCrossedSnapshot",
            &MarketSnapshotReader_RejectsCrossedSnapshot
        },
        {
            "MarketSnapshotReader_RejectsNotUsableForDepth",
            &MarketSnapshotReader_RejectsNotUsableForDepth
        },
        {
            "MarketSnapshotReader_RequiresUsableForSignalWhenConfigured",
            &MarketSnapshotReader_RequiresUsableForSignalWhenConfigured
        },
        {
            "MarketSnapshotReader_UsesConsistencyGuard",
            &MarketSnapshotReader_UsesConsistencyGuard
        },
        {
            "MarketSnapshotReader_MarketStateViewAdapterReadsSnapshot",
            &MarketSnapshotReader_MarketStateViewAdapterReadsSnapshot
        },
        {
            "MarketSnapshotReader_RejectsStaleWhenScanNowExceedsMaxAge",
            &MarketSnapshotReader_RejectsStaleWhenScanNowExceedsMaxAge
        }
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
