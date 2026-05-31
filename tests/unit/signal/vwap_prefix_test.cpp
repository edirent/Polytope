#include "engine/signal/pricing/VWAPPrecheck.h"
#include "engine/signal/scan/BundleRuntimePlan.h"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::oracle::CandidateBundle;
using trading_engine::oracle::Side;
using trading_engine::signal::BundleRuntimePlan;
using trading_engine::signal::CostFailureReason;
using trading_engine::signal::DepthReadResult;
using trading_engine::signal::ExecutableBookSide;
using trading_engine::signal::SnapshotBatchReadResult;
using trading_engine::signal::VWAPPrecheck;
using trading_engine::state::MarketDepthView;
using trading_engine::state::MarketStateSnapshot;
using trading_engine::state::PriceLevel;
using trading_engine::state::build_depth_prefix;
using trading_engine::state::market_depth_view_from_snapshot;

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

MarketStateSnapshot snapshot_with_asks(
    std::initializer_list<PriceLevel> asks
) {
    MarketStateSnapshot snapshot;
    snapshot.entity_id = "asset-yes";
    snapshot.market_id = "m1";
    snapshot.live = true;
    snapshot.usable_for_depth = true;
    snapshot.has_ask = asks.size() > 0;

    std::uint32_t index = 0;
    for (const auto& ask : asks) {
        snapshot.asks[index++] = ask;
    }
    snapshot.ask_count = index;
    return snapshot;
}

CandidateBundle bundle(std::int64_t qty) {
    CandidateBundle out;
    out.bundle_id = 11;
    out.leg_count = 1;
    out.legs[0].market_id = "m1";
    out.legs[0].asset_id = "asset-yes";
    out.legs[0].side = Side::Buy;
    out.legs[0].quantity_lots = qty;
    out.legs[0].max_price_tick = 1'000'000;
    return out;
}

BundleRuntimePlan plan_for(
    const CandidateBundle& candidate,
    std::int64_t ratio_qty_lots
) {
    BundleRuntimePlan plan;
    plan.bundle = &candidate;
    plan.bundle_id = candidate.bundle_id;
    plan.leg_count = 1;
    plan.asset_ids[0] = &candidate.legs[0].asset_id;
    plan.market_ids[0] = &candidate.legs[0].market_id;
    plan.asset_indices[0] = 7;
    plan.sides[0] = Side::Buy;
    plan.executable_sides[0] = ExecutableBookSide::Asks;
    plan.ratio_qty_lots[0] = ratio_qty_lots;
    return plan;
}

DepthReadResult depth_result_from_snapshot(const MarketStateSnapshot& snapshot) {
    DepthReadResult result;
    result.ok = true;
    result.count = 1;
    result.depth_views[0] = market_depth_view_from_snapshot(snapshot, 7);
    return result;
}

SnapshotBatchReadResult snapshot_result(const MarketStateSnapshot& snapshot) {
    SnapshotBatchReadResult result;
    result.ok = true;
    result.snapshot_count = 1;
    result.snapshots[0] = snapshot;
    return result;
}

void VWAPPrefix_ExactFirstLevel() {
    const auto snapshot = snapshot_with_asks({
        PriceLevel{.price_tick = 500'000, .price = 0.50, .size = 10.0},
        PriceLevel{.price_tick = 550'000, .price = 0.55, .size = 5.0}
    });
    const auto candidate = bundle(10);
    const auto plan = plan_for(candidate, 10);

    const auto result =
        VWAPPrecheck{}.price_runtime_plan(plan, depth_result_from_snapshot(snapshot));

    expect_true(result.executable, "executable");
    expect_equal(result.total_cost_tick, 5'000'000LL, "cost");
    expect_equal(result.fixed_legs[0].vwap_price_tick, 500'000LL, "vwap");
    expect_equal(result.fixed_legs[0].worst_price_tick, 500'000LL, "worst");
}

void VWAPPrefix_MultiLevel() {
    const auto snapshot = snapshot_with_asks({
        PriceLevel{.price_tick = 500'000, .price = 0.50, .size = 10.0},
        PriceLevel{.price_tick = 550'000, .price = 0.55, .size = 10.0}
    });
    const auto candidate = bundle(15);
    const auto plan = plan_for(candidate, 15);

    const auto result =
        VWAPPrecheck{}.price_runtime_plan(plan, depth_result_from_snapshot(snapshot));

    expect_true(result.executable, "executable");
    expect_equal(result.total_cost_tick, 7'750'000LL, "cost");
    expect_equal(result.fixed_legs[0].vwap_price_tick, 516'666LL, "vwap");
    expect_equal(result.fixed_legs[0].worst_price_tick, 550'000LL, "worst");
}

void VWAPPrefix_InsufficientDepth() {
    const auto snapshot = snapshot_with_asks({
        PriceLevel{.price_tick = 500'000, .price = 0.50, .size = 10.0}
    });
    const auto candidate = bundle(25);
    const auto plan = plan_for(candidate, 25);

    const auto result =
        VWAPPrecheck{}.price_runtime_plan(plan, depth_result_from_snapshot(snapshot));

    expect_false(result.executable, "executable");
    expect_equal(
        result.failure_reason,
        CostFailureReason::InsufficientDepth,
        "failure"
    );
}

void VWAPPrefix_EqualsOldLinearVWAP() {
    const auto snapshot = snapshot_with_asks({
        PriceLevel{.price_tick = 500'000, .price = 0.50, .size = 10.0},
        PriceLevel{.price_tick = 550'000, .price = 0.55, .size = 10.0},
        PriceLevel{.price_tick = 600'000, .price = 0.60, .size = 10.0}
    });
    const auto candidate = bundle(21);
    const auto plan = plan_for(candidate, 21);

    const auto prefix_result =
        VWAPPrecheck{}.price_runtime_plan(plan, depth_result_from_snapshot(snapshot));
    const auto linear_result =
        VWAPPrecheck{}.price_runtime_plan(plan, snapshot_result(snapshot));

    expect_true(prefix_result.executable, "prefix executable");
    expect_true(linear_result.executable, "linear executable");
    expect_equal(
        prefix_result.total_cost_tick,
        linear_result.total_cost_tick,
        "total cost"
    );
    expect_equal(
        prefix_result.fixed_legs[0].vwap_price_tick,
        linear_result.fixed_legs[0].vwap_price_tick,
        "vwap"
    );
    expect_equal(
        prefix_result.fixed_legs[0].worst_price_tick,
        linear_result.fixed_legs[0].worst_price_tick,
        "worst"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"VWAPPrefix_ExactFirstLevel", &VWAPPrefix_ExactFirstLevel},
        {"VWAPPrefix_MultiLevel", &VWAPPrefix_MultiLevel},
        {"VWAPPrefix_InsufficientDepth", &VWAPPrefix_InsufficientDepth},
        {"VWAPPrefix_EqualsOldLinearVWAP", &VWAPPrefix_EqualsOldLinearVWAP},
    };
    return test_map;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <test-name>\n";
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
        std::cerr << argv[1] << " failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
