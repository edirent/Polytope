#include "tests/integration/decision_fastpath/fastpath_test_helpers.h"

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

void FastPathFallback_SellLegFallsBackToGeneric() {
    oracle::CandidateBundle bundle;
    bundle.bundle_id = 71;
    bundle.guaranteed_payout_tick = 1'000'000;
    bundle.min_edge_tick = 1;
    bundle.leg_count = 1;
    bundle.legs[0] = oracle::BundleLeg{
        .market_id = "market",
        .asset_id = "asset",
        .side = oracle::Side::Sell,
        .quantity_lots = 1,
        .max_price_tick = 1'000'000
    };

    signal::BundleRuntimePlan plan;
    plan.bundle = &bundle;
    plan.bundle_id = bundle.bundle_id;
    plan.bundle_hash = 7'071;
    plan.oracle_artifact_hash = 9'071;
    plan.constraint_hash = 8'071;
    plan.leg_count = 1;
    plan.unique_asset_count = 1;
    plan.market_ids[0] = &bundle.legs[0].market_id;
    plan.asset_ids[0] = &bundle.legs[0].asset_id;
    plan.asset_indices[0] = 71;
    plan.unique_asset_ids[0] = &bundle.legs[0].asset_id;
    plan.unique_asset_indices[0] = 71;
    plan.sides[0] = oracle::Side::Sell;
    plan.executable_sides[0] = signal::ExecutableBookSide::Unsupported;
    plan.ratio_qty_lots[0] = 1;
    plan.guaranteed_payout_tick = 1'000'000;
    plan.min_unit_edge_tick = 1;
    plan.min_total_edge_tick = 1;

    const std::array<signal::BundleRuntimePlan, 1> plans{plan};
    fast::FixedShapeKernelRegistry registry;
    registry.build_from_runtime_plans(plans);

    const auto policy = decision_fastpath_test::policy();
    const std::array depths{
        decision_fastpath_test::make_depth_view(71, 400'000, 10.0)
    };
    fast::EventLocalDecisionPipelineConfig config;
    config.expected_artifact_hash = plan.oracle_artifact_hash;
    config.expected_constraint_hash = plan.constraint_hash;
    config.expected_policy_hash = policy.policy_hash;
    config.mode = fast::EventLocalPipelineMode::PaperAuthoritative;
    config.fast_path.mode = fast::FastPathMode::PaperAuthoritative;
    config.fast_path.enable_fixed_buy_kernel = true;
    fast::EventLocalDecisionPipeline pipeline{&registry, config};

    const auto result = pipeline.process({
        .now_ns = 2'000,
        .dirty_asset_index = 71,
        .depth_views = depths.data(),
        .depth_view_count = static_cast<std::uint16_t>(depths.size()),
        .policy = &policy,
        .ledger = nullptr,
        .current_true_mask = 0,
        .current_false_mask = 0
    });

    expect_true(result.fallback_required, "fallback");
    expect_equal(result.reject_reason, fast::FastPathRejectReason::SellLeg, "reason");
}

void FastPathFallback_KillSwitchDisablesFastPath() {
    auto policy = decision_fastpath_test::policy();
    policy.kill_switch_enabled = true;
    policy = trading_engine::risk::with_computed_policy_hash(policy);
    fast::FixedShapeKernelRegistry registry;
    fast::EventLocalDecisionPipelineConfig config;
    config.fast_path.mode = fast::FastPathMode::PaperAuthoritative;
    config.fast_path.enable_fixed_buy_kernel = true;
    fast::EventLocalDecisionPipeline pipeline{&registry, config};
    const auto result = pipeline.process({
        .now_ns = 2'000,
        .dirty_asset_index = 1,
        .depth_views = nullptr,
        .depth_view_count = 0,
        .policy = &policy,
        .ledger = nullptr,
        .current_true_mask = 0,
        .current_false_mask = 0
    });
    expect_true(result.fallback_required, "fallback");
    expect_equal(
        result.reject_reason,
        fast::FastPathRejectReason::PolicyIncompatible,
        "reason"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"FastPathFallback_SellLegFallsBackToGeneric",
         &FastPathFallback_SellLegFallsBackToGeneric},
        {"FastPathFallback_KillSwitchDisablesFastPath",
         &FastPathFallback_KillSwitchDisablesFastPath},
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
