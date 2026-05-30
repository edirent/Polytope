#include "engine/signal/reader/SnapshotConsistencyGuard.h"

#include "state/quality/BookQualityState.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using trading_engine::signal::IntentStatus;
using trading_engine::signal::SignalConfig;
using trading_engine::signal::SnapshotConsistencyGuard;
using trading_engine::signal::SnapshotConsistencyMode;
using trading_engine::state::BookQuality;
using trading_engine::state::MarketStateSnapshot;

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

MarketStateSnapshot snapshot(
    const std::string& asset_id,
    std::uint64_t version,
    std::uint64_t last_book_update_ns
) {
    MarketStateSnapshot out;
    out.entity_id = asset_id;
    out.market_id = "m1";
    out.version = version;
    out.last_book_update_ns = last_book_update_ns;
    out.live = true;
    out.has_bid = true;
    out.has_ask = true;
    out.best_bid_tick = 490'000;
    out.best_ask_tick = 500'000;
    out.bid_count = 1;
    out.ask_count = 1;
    out.state_hash = version * 17;
    out.quality = BookQuality::Good;
    out.usable_for_depth = true;
    out.usable_for_signal = true;
    return out;
}

void SnapshotGuard_AcceptsConsistentSnapshots() {
    SignalConfig config;
    const std::vector<MarketStateSnapshot> snapshots{
        snapshot("asset_a", 100, 1'000),
        snapshot("asset_b", 105, 1'500)
    };

    const auto result =
        SnapshotConsistencyGuard{}.check(snapshots, 2'000, config);

    expect_true(result.ok, "guard ok: " + result.error);
    expect_equal(result.version.min_book_version, 100ULL, "min version");
    expect_equal(result.version.max_book_version, 105ULL, "max version");
    expect_equal(result.version.read_ts_ns, 2'000ULL, "read ts");
}

void SnapshotGuard_RejectsMissingSnapshot() {
    SignalConfig config;
    const std::vector<MarketStateSnapshot> snapshots;

    const auto result =
        SnapshotConsistencyGuard{}.check(snapshots, 0, config);

    expect_false(result.ok, "guard ok");
    expect_equal(
        result.rejection_status,
        IntentStatus::RejectedMissingSnapshot,
        "status"
    );
}

void SnapshotGuard_RejectsStaleSnapshot() {
    SignalConfig config;
    config.max_lob_age_ns = 1'000;
    const std::vector<MarketStateSnapshot> snapshots{
        snapshot("asset_a", 100, 1'000)
    };

    const auto result =
        SnapshotConsistencyGuard{}.check(snapshots, 3'000, config);

    expect_false(result.ok, "guard ok");
    expect_equal(
        result.rejection_status,
        IntentStatus::RejectedBadMarketState,
        "status"
    );
}

void SnapshotGuard_RejectsCrossedSnapshot() {
    SignalConfig config;
    auto crossed = snapshot("asset_a", 100, 1'000);
    crossed.crossed = true;
    crossed.quality = BookQuality::Crossed;
    const std::vector<MarketStateSnapshot> snapshots{crossed};

    const auto result =
        SnapshotConsistencyGuard{}.check(snapshots, 1'000, config);

    expect_false(result.ok, "guard ok");
    expect_equal(
        result.rejection_status,
        IntentStatus::RejectedBadMarketState,
        "status"
    );
}

void SnapshotGuard_RejectsVersionSkewTooLarge() {
    SignalConfig config;
    config.max_snapshot_version_skew = 10;
    const std::vector<MarketStateSnapshot> snapshots{
        snapshot("asset_a", 100, 1'000),
        snapshot("asset_b", 111, 1'000)
    };

    const auto result =
        SnapshotConsistencyGuard{}.check(snapshots, 1'000, config);

    expect_false(result.ok, "guard ok");
    expect_equal(
        result.rejection_status,
        IntentStatus::RejectedBadMarketState,
        "status"
    );
}

void SnapshotGuard_AllowsBoundedSkew() {
    SignalConfig config;
    config.max_snapshot_version_skew = 10;
    config.consistency_mode = SnapshotConsistencyMode::BoundedSkew;
    const std::vector<MarketStateSnapshot> snapshots{
        snapshot("asset_a", 100, 1'000),
        snapshot("asset_b", 110, 1'000)
    };

    const auto result =
        SnapshotConsistencyGuard{}.check(snapshots, 1'000, config);

    expect_true(result.ok, "guard ok: " + result.error);
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "SnapshotGuard_AcceptsConsistentSnapshots",
            &SnapshotGuard_AcceptsConsistentSnapshots
        },
        {
            "SnapshotGuard_RejectsMissingSnapshot",
            &SnapshotGuard_RejectsMissingSnapshot
        },
        {
            "SnapshotGuard_RejectsStaleSnapshot",
            &SnapshotGuard_RejectsStaleSnapshot
        },
        {
            "SnapshotGuard_RejectsCrossedSnapshot",
            &SnapshotGuard_RejectsCrossedSnapshot
        },
        {
            "SnapshotGuard_RejectsVersionSkewTooLarge",
            &SnapshotGuard_RejectsVersionSkewTooLarge
        },
        {
            "SnapshotGuard_AllowsBoundedSkew",
            &SnapshotGuard_AllowsBoundedSkew
        }
    };
    return test_map;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <test-name>\n";
        return 2;
    }

    const std::string name = argv[1];
    const auto it = tests().find(name);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << name << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& ex) {
        std::cerr << name << " failed: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
