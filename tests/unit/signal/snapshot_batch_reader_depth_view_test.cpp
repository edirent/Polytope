#include "engine/signal/pricing/VWAPPrecheck.h"
#include "engine/signal/reader/SnapshotBatchReader.h"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::oracle::BundleLeg;
using trading_engine::oracle::CandidateBundle;
using trading_engine::oracle::Side;
using trading_engine::signal::BundleRuntimePlan;
using trading_engine::signal::CostFailureReason;
using trading_engine::signal::DepthReadResult;
using trading_engine::signal::ExecutableBookSide;
using trading_engine::signal::SignalConfig;
using trading_engine::signal::VWAPPrecheck;
using trading_engine::signal::kMaxIntentLegs;
using trading_engine::signal::validate_plan_depth_views;
using trading_engine::state::MarketDepthView;
using trading_engine::state::PriceLevel;

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

CandidateBundle bundle() {
    CandidateBundle out;
    out.bundle_id = 1;
    out.leg_count = 1;
    out.legs[0] = BundleLeg{
        .market_id = "m1",
        .asset_id = "asset-yes",
        .side = Side::Buy,
        .quantity_lots = 5,
        .max_price_tick = 1'000'000
    };
    return out;
}

BundleRuntimePlan runtime_plan(const CandidateBundle& bundle) {
    BundleRuntimePlan plan;
    plan.bundle = &bundle;
    plan.bundle_id = bundle.bundle_id;
    plan.leg_count = 1;
    plan.unique_asset_count = 1;
    plan.asset_ids[0] = &bundle.legs[0].asset_id;
    plan.market_ids[0] = &bundle.legs[0].market_id;
    plan.unique_asset_ids[0] = &bundle.legs[0].asset_id;
    plan.asset_indices[0] = 3;
    plan.unique_asset_indices[0] = 3;
    plan.sides[0] = Side::Buy;
    plan.executable_sides[0] = ExecutableBookSide::Asks;
    plan.ratio_qty_lots[0] = 5;
    return plan;
}

MarketDepthView depth_view() {
    MarketDepthView view;
    view.asset_index = 3;
    view.book_version = 10;
    view.snapshot_version_hash = 9001;
    view.last_ws_recv_ns = 10'000;
    view.usable_for_depth = true;
    view.ask_count = 1;
    view.asks[0] = PriceLevel{.price_tick = 500'000, .price = 0.50, .size = 10.0};
    return view;
}

void DepthBatchReadValidatesRequiredAssets() {
    const auto candidate = bundle();
    const auto plan = runtime_plan(candidate);
    std::array<MarketDepthView, kMaxIntentLegs> depth{};
    depth[0] = depth_view();

    SignalConfig config;
    const auto result = validate_plan_depth_views(plan, config, depth, 1, 10'000);

    expect_true(result.ok, "depth read ok");
    expect_equal(result.count, static_cast<std::uint16_t>(1), "depth count");
    expect_equal(result.snapshot_version.max_book_version, 10ULL, "version");
    expect_equal(
        result.snapshot_version_hash,
        result.snapshot_version.combined_hash,
        "combined hash"
    );
}

void SignalUsesDepthViewForVWAP() {
    const auto candidate = bundle();
    const auto plan = runtime_plan(candidate);
    std::array<MarketDepthView, kMaxIntentLegs> depth{};
    depth[0] = depth_view();

    SignalConfig config;
    const auto read = validate_plan_depth_views(plan, config, depth, 1, 10'000);
    const auto cost = VWAPPrecheck{}.price_runtime_plan(plan, read);

    expect_true(cost.executable, "cost executable");
    expect_equal(cost.failure_reason, CostFailureReason::None, "failure");
    expect_equal(cost.bundle_qty, 2LL, "bundle qty");
    expect_equal(cost.total_cost_tick, 5'000'000LL, "cost");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"DepthBatchReadValidatesRequiredAssets",
         &DepthBatchReadValidatesRequiredAssets},
        {"SignalUsesDepthViewForVWAP", &SignalUsesDepthViewForVWAP},
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
