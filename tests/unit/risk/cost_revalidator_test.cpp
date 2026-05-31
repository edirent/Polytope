#include "engine/risk/reprice/CostRevalidator.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using trading_engine::risk::CostRevalidator;
using trading_engine::risk::RiskDecisionType;
using trading_engine::risk::RiskPolicySnapshot;
using trading_engine::risk::RiskStageTimings;
using trading_engine::risk::RiskVWAPMode;
using trading_engine::signal::OpportunityIntent;
using trading_engine::state::MarketStateSnapshot;

inline constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

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

void mix_u64(std::uint64_t value, std::uint64_t* hash) noexcept {
    for (std::uint8_t i = 0; i < 8; ++i) {
        *hash ^= static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
        *hash *= kFnvPrime;
    }
}

void mix_string(const std::string& value, std::uint64_t* hash) noexcept {
    for (const unsigned char c : value) {
        *hash ^= c;
        *hash *= kFnvPrime;
    }
}

std::uint64_t combined_hash(const MarketStateSnapshot& snapshot) {
    std::uint64_t hash = kFnvOffset;
    mix_string(snapshot.entity_id, &hash);
    mix_u64(snapshot.version, &hash);
    mix_u64(
        snapshot.snapshot_version_hash != 0
            ? snapshot.snapshot_version_hash
            : snapshot.state_hash,
        &hash
    );
    mix_u64(snapshot.last_book_update_ns, &hash);
    return hash;
}

MarketStateSnapshot snapshot(
    std::string asset_id,
    std::int64_t ask_price_tick,
    double ask_size
) {
    MarketStateSnapshot out;
    out.entity_id = std::move(asset_id);
    out.market_id = "market-1";
    out.version = 10;
    out.last_book_update_ns = 1'000;
    out.snapshot_version_hash = 111;
    out.live = true;
    out.usable_for_depth = true;
    out.has_ask = true;
    out.ask_count = 1;
    out.asks[0].price_tick = ask_price_tick;
    out.asks[0].size = ask_size;
    return out;
}

OpportunityIntent intent() {
    OpportunityIntent out;
    out.intent_id = 1;
    out.bundle_id = 2;
    out.bundle_qty = 10;
    out.original_bundle_qty = 10;
    out.estimated_cost_tick = 1'000;
    out.estimated_fee_tick = 3;
    out.slippage_buffer_tick = 4;
    out.latency_buffer_tick = 5;
    out.leg_count = 1;
    out.legs[0].market_id = "market-1";
    out.legs[0].asset_id = "asset-1";
    out.legs[0].quantity_lots = 1;
    out.legs[0].enough_depth = true;
    return out;
}

RiskPolicySnapshot permissive_policy() {
    RiskPolicySnapshot policy;
    policy.max_allowed_cost_drift_tick = 10'000;
    return policy;
}

void CostRevalidator_RecomputesVWAPFromLatestSnapshot() {
    auto in = intent();
    in.estimated_cost_tick = 1'000;
    const std::vector<MarketStateSnapshot> snapshots{
        snapshot("asset-1", 120, 10.0)
    };

    const auto result =
        CostRevalidator{}.revalidate(in, snapshots, permissive_policy());

    expect_true(result.ok, "ok");
    expect_equal(result.risk_total_cost_tick, 1'200LL, "risk cost");
    expect_equal(result.cost_drift_tick, 200LL, "cost drift");
    expect_equal(result.risk_bundle_qty, 10LL, "bundle qty");
    expect_equal(result.fee_tick, 3LL, "fee");
    expect_equal(result.slippage_buffer_tick, 4LL, "slippage");
    expect_equal(result.latency_buffer_tick, 5LL, "latency");
    expect_equal(result.leg_count, static_cast<std::uint16_t>(1), "leg count");
    expect_equal(
        result.legs[0].requested_qty_lots,
        10LL,
        "leg requested"
    );
    expect_equal(
        result.legs[0].executable_qty_lots,
        10LL,
        "leg executable"
    );
    expect_equal(
        result.legs[0].depth_margin_bps,
        10'000LL,
        "leg depth margin"
    );
    expect_true(result.legs[0].enough_depth, "leg enough depth");
    expect_equal(
        result.vwap_mode,
        RiskVWAPMode::RecomputedFromSnapshot,
        "mode"
    );
}

void CostRevalidator_ReusesSignalSnapshotWhenSnapshotHashMatches() {
    const auto latest = snapshot("asset-1", 120, 10.0);
    auto in = intent();
    in.snapshot_version_hash = combined_hash(latest);
    in.expires_at_ns = 2'000;
    in.estimated_cost_tick = 777;
    in.max_leg_slippage_tick = 6;
    in.legs[0].requested_qty_lots = 10;
    in.legs[0].executable_qty_lots = 12;
    in.legs[0].depth_margin_bps = 12'000;

    RiskStageTimings timings;
    const auto result = CostRevalidator{}.revalidate(
        in,
        std::vector<MarketStateSnapshot>{latest},
        permissive_policy(),
        1'500,
        combined_hash(latest),
        &timings
    );

    expect_true(result.ok, "ok");
    expect_equal(result.risk_total_cost_tick, 777LL, "risk cost");
    expect_equal(result.cost_drift_tick, 0LL, "cost drift");
    expect_equal(result.risk_bundle_qty, 10LL, "bundle qty");
    expect_equal(result.max_leg_slippage_tick, 6LL, "slippage");
    expect_equal(result.leg_count, static_cast<std::uint16_t>(1), "leg count");
    expect_equal(
        result.legs[0].requested_qty_lots,
        10LL,
        "leg requested"
    );
    expect_equal(
        result.legs[0].executable_qty_lots,
        12LL,
        "leg executable"
    );
    expect_equal(
        result.legs[0].depth_margin_bps,
        12'000LL,
        "leg margin"
    );
    expect_equal(result.vwap_mode, RiskVWAPMode::ReuseSignalSnapshot, "mode");
    expect_true(timings.vwap_revalidator_ns > 0, "vwap timing");
}

void CostRevalidator_ReusesPrecomputedSnapshotVersionHash() {
    const auto latest = snapshot("asset-1", 120, 10.0);
    auto in = intent();
    in.snapshot_version_hash = combined_hash(latest);
    in.expires_at_ns = 2'000;
    in.estimated_cost_tick = 777;

    const auto result = CostRevalidator{}.revalidate(
        in,
        std::vector<MarketStateSnapshot>{latest},
        permissive_policy(),
        1'500,
        combined_hash(latest),
        nullptr
    );

    expect_true(result.ok, "ok");
    expect_equal(result.risk_total_cost_tick, 777LL, "risk cost");
    expect_equal(result.cost_drift_tick, 0LL, "cost drift");
    expect_equal(result.vwap_mode, RiskVWAPMode::ReuseSignalSnapshot, "mode");
}

void CostRevalidator_RecomputesWhenSnapshotHashDiffers() {
    const auto latest = snapshot("asset-1", 120, 10.0);
    auto in = intent();
    in.snapshot_version_hash = combined_hash(latest) ^ 0x1234ULL;
    in.expires_at_ns = 2'000;
    in.estimated_cost_tick = 1'000;

    const auto result = CostRevalidator{}.revalidate(
        in,
        std::vector<MarketStateSnapshot>{latest},
        permissive_policy(),
        1'500,
        nullptr
    );

    expect_true(result.ok, "ok");
    expect_equal(result.risk_total_cost_tick, 1'200LL, "risk cost");
    expect_equal(
        result.vwap_mode,
        RiskVWAPMode::RecomputedFromSnapshot,
        "mode"
    );
}

void CostRevalidator_DoesNotReuseStaleSnapshot() {
    const auto latest = snapshot("asset-1", 120, 10.0);
    auto in = intent();
    in.snapshot_version_hash = combined_hash(latest);
    in.expires_at_ns = 3'000;
    in.estimated_cost_tick = 777;
    auto policy = permissive_policy();
    policy.max_book_age_ns = 100;

    const auto result = CostRevalidator{}.revalidate(
        in,
        std::vector<MarketStateSnapshot>{latest},
        policy,
        2'000,
        combined_hash(latest),
        nullptr
    );

    expect_true(result.ok, "ok");
    expect_equal(result.risk_total_cost_tick, 1'200LL, "risk cost");
    expect_equal(
        result.vwap_mode,
        RiskVWAPMode::RecomputedFromSnapshot,
        "mode"
    );
}

void CostRevalidator_RejectsInsufficientDepth() {
    auto bad_snapshot = snapshot("asset-1", 120, 0.0);
    bad_snapshot.has_ask = false;
    bad_snapshot.ask_count = 0;

    const auto result = CostRevalidator{}.revalidate(
        intent(),
        std::vector<MarketStateSnapshot>{bad_snapshot},
        permissive_policy()
    );

    expect_false(result.ok, "ok");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectInsufficientDepth,
        "rejection"
    );
}

void CostRevalidator_RejectsCostDriftTooHigh() {
    RiskPolicySnapshot policy;
    policy.max_allowed_cost_drift_tick = 50;

    const auto result = CostRevalidator{}.revalidate(
        intent(),
        std::vector<MarketStateSnapshot>{snapshot("asset-1", 120, 10.0)},
        policy
    );

    expect_false(result.ok, "ok");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectCostDrift,
        "rejection"
    );
    expect_equal(result.cost_drift_tick, 200LL, "drift");
}

void CostRevalidator_AllowsSmallCostDrift() {
    RiskPolicySnapshot policy;
    policy.max_allowed_cost_drift_tick = 250;

    const auto result = CostRevalidator{}.revalidate(
        intent(),
        std::vector<MarketStateSnapshot>{snapshot("asset-1", 120, 10.0)},
        policy
    );

    expect_true(result.ok, "ok");
    expect_equal(result.rejection, RiskDecisionType::Approve, "rejection");
    expect_equal(result.cost_drift_tick, 200LL, "drift");
}

void CostRevalidator_RejectsReducedBundleQty() {
    const auto result = CostRevalidator{}.revalidate(
        intent(),
        std::vector<MarketStateSnapshot>{snapshot("asset-1", 100, 9.0)},
        permissive_policy()
    );

    expect_false(result.ok, "ok");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectReducedBundleQty,
        "rejection"
    );
    expect_equal(result.risk_bundle_qty, 9LL, "risk bundle qty");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "CostRevalidator_RecomputesVWAPFromLatestSnapshot",
            &CostRevalidator_RecomputesVWAPFromLatestSnapshot
        },
        {
            "CostRevalidator_RejectsInsufficientDepth",
            &CostRevalidator_RejectsInsufficientDepth
        },
        {
            "CostRevalidator_ReusesSignalSnapshotWhenSnapshotHashMatches",
            &CostRevalidator_ReusesSignalSnapshotWhenSnapshotHashMatches
        },
        {
            "CostRevalidator_ReusesPrecomputedSnapshotVersionHash",
            &CostRevalidator_ReusesPrecomputedSnapshotVersionHash
        },
        {
            "CostRevalidator_RecomputesWhenSnapshotHashDiffers",
            &CostRevalidator_RecomputesWhenSnapshotHashDiffers
        },
        {
            "CostRevalidator_DoesNotReuseStaleSnapshot",
            &CostRevalidator_DoesNotReuseStaleSnapshot
        },
        {
            "CostRevalidator_RejectsCostDriftTooHigh",
            &CostRevalidator_RejectsCostDriftTooHigh
        },
        {
            "CostRevalidator_AllowsSmallCostDrift",
            &CostRevalidator_AllowsSmallCostDrift
        },
        {
            "CostRevalidator_RejectsReducedBundleQty",
            &CostRevalidator_RejectsReducedBundleQty
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
