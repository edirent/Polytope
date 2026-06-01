#include "tests/integration/decision_fastpath/fastpath_test_helpers.h"

#include "engine/signal/scan/BundleRuntimePlan.h"
#include "oracle/public/CandidateBundle.h"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

namespace fast = trading_engine::decision_fastpath;
namespace oracle = trading_engine::oracle;
namespace signal = trading_engine::signal;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

oracle::CandidateBundle bundle() {
    oracle::CandidateBundle out;
    out.bundle_id = 39;
    out.guaranteed_payout_tick = 1'000'000;
    out.min_edge_tick = 1;
    out.leg_count = 1;
    out.legs[0] = oracle::BundleLeg{
        .market_id = "market39",
        .asset_id = "asset39",
        .side = oracle::Side::Buy,
        .quantity_lots = 1,
        .max_price_tick = 1'000'000
    };
    return out;
}

signal::BundleRuntimePlan plan_for(const oracle::CandidateBundle& bundle) {
    signal::BundleRuntimePlan plan;
    plan.bundle = &bundle;
    plan.bundle_id = bundle.bundle_id;
    plan.bundle_hash = 7'039;
    plan.oracle_artifact_hash = 9'039;
    plan.constraint_hash = 8'039;
    plan.leg_count = 1;
    plan.unique_asset_count = 1;
    plan.market_ids[0] = &bundle.legs[0].market_id;
    plan.asset_ids[0] = &bundle.legs[0].asset_id;
    plan.asset_indices[0] = 39;
    plan.unique_asset_ids[0] = &bundle.legs[0].asset_id;
    plan.unique_asset_indices[0] = 39;
    plan.sides[0] = oracle::Side::Buy;
    plan.executable_sides[0] = signal::ExecutableBookSide::Asks;
    plan.ratio_qty_lots[0] = 1;
    plan.max_price_ticks[0] = 1'000'000;
    plan.guaranteed_payout_tick = bundle.guaranteed_payout_tick;
    plan.min_unit_edge_tick = bundle.min_edge_tick;
    plan.min_total_edge_tick = bundle.min_edge_tick;
    return plan;
}

void FastPathShadowMarket39_MismatchZero() {
    const auto candidate = bundle();
    const std::array<signal::BundleRuntimePlan, 1> plans{plan_for(candidate)};
    fast::FixedShapeKernelRegistry registry;
    registry.build_from_runtime_plans(plans);

    const auto policy = decision_fastpath_test::policy();
    const std::array depths{
        decision_fastpath_test::make_depth_view(39, 400'000, 10.0)
    };

    decision_fastpath_test::expect_shadow_matches_generic_for_all_dirty_assets(
        registry,
        policy,
        depths
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"FastPathShadowMarket39_MismatchZero",
         &FastPathShadowMarket39_MismatchZero},
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
