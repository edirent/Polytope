#include "engine/decision_fastpath/kernel/FixedShapeKernelSpec.h"

#include "engine/signal/pricing/SideResolver.h"

#include <exception>
#include <filesystem>
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

std::filesystem::path source_path(const std::string& relative) {
    return std::filesystem::path{POLYTOPE_SOURCE_DIR} / relative;
}

struct RuntimePlanFixture {
    oracle::CandidateBundle bundle_a;
    oracle::CandidateBundle bundle_b;
    signal::BundleRuntimePlan plan_a;
    signal::BundleRuntimePlan plan_b;
    std::array<signal::BundleRuntimePlan, 2> plans{};
};

void fill_plan_from_bundle(
    const oracle::CandidateBundle& bundle,
    std::uint64_t artifact_hash,
    std::uint64_t constraint_hash,
    std::uint32_t first_asset_index,
    signal::BundleRuntimePlan* plan
) {
    signal::SideResolver side_resolver;
    plan->bundle = &bundle;
    plan->bundle_id = bundle.bundle_id;
    plan->bundle_hash = 7'000 + bundle.bundle_id;
    plan->oracle_artifact_hash = artifact_hash;
    plan->constraint_hash = constraint_hash;
    plan->leg_count = bundle.leg_count;
    plan->unique_asset_count = bundle.leg_count;
    plan->guaranteed_payout_tick = bundle.guaranteed_payout_tick;
    plan->min_unit_edge_tick = bundle.min_edge_tick;
    plan->min_total_edge_tick = bundle.min_edge_tick;
    plan->min_edge_bps = 125;

    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        plan->market_ids[i] = &bundle.legs[i].market_id;
        plan->asset_ids[i] = &bundle.legs[i].asset_id;
        plan->asset_indices[i] = first_asset_index + i;
        plan->unique_asset_indices[i] = first_asset_index + i;
        plan->unique_asset_ids[i] = &bundle.legs[i].asset_id;
        plan->sides[i] = bundle.legs[i].side;
        plan->executable_sides[i] = side_resolver.resolve(bundle.legs[i].side);
        plan->ratio_qty_lots[i] = bundle.legs[i].quantity_lots;
        plan->max_price_ticks[i] = bundle.legs[i].max_price_tick;
    }
}

RuntimePlanFixture runtime_fixture() {
    RuntimePlanFixture out;

    out.bundle_a.bundle_id = 101;
    out.bundle_a.guaranteed_payout_tick = 1'000'000;
    out.bundle_a.min_edge_tick = 10;
    out.bundle_a.leg_count = 2;
    out.bundle_a.legs[0] = oracle::BundleLeg{
        .market_id = "market-a",
        .asset_id = "asset-a-yes",
        .side = oracle::Side::Buy,
        .quantity_lots = 2,
        .max_price_tick = 500'000
    };
    out.bundle_a.legs[1] = oracle::BundleLeg{
        .market_id = "market-b",
        .asset_id = "asset-b-yes",
        .side = oracle::Side::Buy,
        .quantity_lots = 3,
        .max_price_tick = 400'000
    };

    out.bundle_b.bundle_id = 102;
    out.bundle_b.guaranteed_payout_tick = 2'000'000;
    out.bundle_b.min_edge_tick = 20;
    out.bundle_b.leg_count = 1;
    out.bundle_b.legs[0] = oracle::BundleLeg{
        .market_id = "market-a",
        .asset_id = "asset-a-yes",
        .side = oracle::Side::Buy,
        .quantity_lots = 5,
        .max_price_tick = 450'000
    };

    fill_plan_from_bundle(out.bundle_a, 9001, 8001, 10, &out.plan_a);
    fill_plan_from_bundle(out.bundle_b, 9001, 8001, 10, &out.plan_b);
    out.plans[0] = out.plan_b;
    out.plans[1] = out.plan_a;
    return out;
}

void FixedShapeKernelRegistry_BuildsFromOracleArtifactDeterministic() {
    signal::OracleArtifactReader artifact_a;
    signal::OracleArtifactReader artifact_b;
    const auto path = source_path("tests/fixtures/oracle/artifact_positive");

    const auto load_a = artifact_a.load(path);
    const auto load_b = artifact_b.load(path);
    expect_true(load_a.ok, "load a");
    expect_true(load_b.ok, "load b");

    fast::FixedShapeKernelRegistry registry_a;
    fast::FixedShapeKernelRegistry registry_b;
    registry_a.build_from_oracle_artifact(artifact_a);
    registry_b.build_from_oracle_artifact(artifact_b);

    expect_true(!registry_a.specs().empty(), "specs");
    expect_equal(registry_a.specs().size(), registry_b.specs().size(), "count");
    for (std::size_t i = 0; i < registry_a.specs().size(); ++i) {
        expect_equal(
            registry_a.specs()[i].kernel_spec_hash,
            registry_b.specs()[i].kernel_spec_hash,
            "kernel hash"
        );
        expect_equal(
            registry_a.specs()[i].kernel_spec_hash,
            fast::hash_fixed_shape_kernel_spec(registry_a.specs()[i]),
            "recomputed hash"
        );
    }
}

void FixedShapeKernelRegistry_PreservesRuntimePlanFields() {
    auto data = runtime_fixture();
    fast::FixedShapeKernelRegistry registry;

    registry.build_from_runtime_plans(data.plans);
    const auto* spec = registry.find(101);

    expect_true(spec != nullptr, "spec found");
    expect_equal(spec->artifact_hash, 9001ULL, "artifact hash");
    expect_equal(spec->constraint_hash, 8001ULL, "constraint hash");
    expect_equal(spec->bundle_hash, 7101ULL, "bundle hash");
    expect_equal(spec->bundle_id, 101ULL, "bundle id");
    expect_equal(spec->leg_count, static_cast<std::uint8_t>(2), "leg count");
    expect_equal(spec->asset_indices[0], 10U, "asset 0");
    expect_equal(spec->asset_indices[1], 11U, "asset 1");
    expect_equal(spec->market_indices[0], 0U, "market 0");
    expect_equal(spec->market_indices[1], 1U, "market 1");
    expect_equal(spec->ratio_qty_lots[0], 2LL, "ratio 0");
    expect_equal(spec->ratio_qty_lots[1], 3LL, "ratio 1");
    expect_equal(spec->guaranteed_payout_tick, 1'000'000LL, "payout");
    expect_equal(spec->min_unit_edge_tick, 10LL, "min unit");
    expect_equal(spec->min_total_edge_tick, 10LL, "min total");
    expect_equal(spec->min_edge_bps, 125LL, "min bps");
    expect_equal(spec->min_bundle_qty, 1LL, "min qty");
    expect_equal(
        spec->kernel_spec_hash,
        fast::hash_fixed_shape_kernel_spec(*spec),
        "hash"
    );
}

void FixedShapeKernelRegistry_FindByBundleId() {
    auto data = runtime_fixture();
    fast::FixedShapeKernelRegistry registry;

    registry.build_from_runtime_plans(data.plans);

    expect_true(registry.find(101) != nullptr, "find 101");
    expect_true(registry.find(102) != nullptr, "find 102");
    expect_true(registry.find(999) == nullptr, "missing");
}

void FixedShapeKernelRegistry_SpecsForAssetIndex() {
    auto data = runtime_fixture();
    fast::FixedShapeKernelRegistry registry;

    registry.build_from_runtime_plans(data.plans);
    const auto specs = registry.specs_for_asset(10);

    expect_equal(specs.size(), static_cast<std::size_t>(2), "asset specs");
    expect_equal(specs[0].bundle_id, 101ULL, "first bundle");
    expect_equal(specs[1].bundle_id, 102ULL, "second bundle");
}

void FixedShapeKernelSpec_HashStable() {
    auto data = runtime_fixture();
    fast::FixedShapeKernelRegistry registry_a;
    fast::FixedShapeKernelRegistry registry_b;

    registry_a.build_from_runtime_plans(data.plans);
    registry_b.build_from_runtime_plans(data.plans);

    const auto* a = registry_a.find(101);
    const auto* b = registry_b.find(101);

    expect_true(a != nullptr, "spec a");
    expect_true(b != nullptr, "spec b");
    expect_equal(a->kernel_spec_hash, b->kernel_spec_hash, "hash");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"FixedShapeKernelRegistry_BuildsFromOracleArtifactDeterministic",
         &FixedShapeKernelRegistry_BuildsFromOracleArtifactDeterministic},
        {"FixedShapeKernelRegistry_PreservesRuntimePlanFields",
         &FixedShapeKernelRegistry_PreservesRuntimePlanFields},
        {"FixedShapeKernelRegistry_FindByBundleId",
         &FixedShapeKernelRegistry_FindByBundleId},
        {"FixedShapeKernelRegistry_SpecsForAssetIndex",
         &FixedShapeKernelRegistry_SpecsForAssetIndex},
        {"FixedShapeKernelSpec_HashStable", &FixedShapeKernelSpec_HashStable},
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
