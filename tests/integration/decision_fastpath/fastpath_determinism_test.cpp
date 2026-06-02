#include "tests/integration/decision_fastpath/fastpath_test_helpers.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

namespace fast = trading_engine::decision_fastpath;
namespace risk = trading_engine::risk;

#ifndef POLYTOPE_SOURCE_DIR
#define POLYTOPE_SOURCE_DIR "."
#endif

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_equal(
    std::uint64_t actual,
    std::uint64_t expected,
    const std::string& field
) {
    if (actual != expected) {
        fail("mismatch: " + field);
    }
}

void FastPathDeterminism_OutputHashStable() {
    const std::filesystem::path artifact{
        std::filesystem::path{POLYTOPE_SOURCE_DIR} /
        "runs/worldcup_30615_full_20260530_062519/"
        "oracle_artifact_30615_top8"
    };
    if (!std::filesystem::exists(artifact / "checksums.txt")) {
        std::cout << "SKIP: worldcup artifact fixture missing\n";
        return;
    }
    const auto registry =
        decision_fastpath_test::registry_from_artifact(artifact);
    if (registry.specs().empty()) {
        fail("worldcup artifact has no fastpath specs");
    }

    const auto policy = decision_fastpath_test::policy();
    const risk::RiskLedgerSnapshot ledger;
    const auto depths = decision_fastpath_test::depths_for_specs(
        registry.specs()
    );
    fast::EventLocalDecisionPipelineConfig config;
    config.expected_policy_hash = policy.policy_hash;
    config.expected_artifact_hash = registry.specs().front().artifact_hash;
    config.expected_constraint_hash = registry.specs().front().constraint_hash;
    config.mode = fast::EventLocalPipelineMode::ShadowCompare;
    config.fast_path.mode = fast::FastPathMode::ShadowCompare;
    config.fast_path.enable_fixed_buy_kernel = true;

    fast::EventLocalDecisionPipeline pipeline{&registry, config};
    const auto dirty = registry.specs().front().asset_indices[0];
    const auto first = pipeline.process({
        .now_ns = 2'000,
        .dirty_asset_index = dirty,
        .depth_views = depths.data(),
        .depth_view_count = static_cast<std::uint16_t>(depths.size()),
        .policy = &policy,
        .ledger = &ledger,
        .current_true_mask = 0,
        .current_false_mask = 0
    });
    const auto second = pipeline.process({
        .now_ns = 2'000,
        .dirty_asset_index = dirty,
        .depth_views = depths.data(),
        .depth_view_count = static_cast<std::uint16_t>(depths.size()),
        .policy = &policy,
        .ledger = &ledger,
        .current_true_mask = 0,
        .current_false_mask = 0
    });

    expect_equal(first.output_hash, second.output_hash, "output hash");
    expect_equal(first.intent.intent_id, second.intent.intent_id, "intent id");
    expect_equal(first.plan.plan_id, second.plan.plan_id, "plan id");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"FastPathDeterminism_OutputHashStable",
         &FastPathDeterminism_OutputHashStable},
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
