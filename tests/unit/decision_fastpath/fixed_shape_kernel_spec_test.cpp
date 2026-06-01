#include "engine/decision_fastpath/kernel/FixedShapeKernelSpec.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

namespace fast = trading_engine::decision_fastpath;
namespace oracle = trading_engine::oracle;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
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

fast::FixedShapeKernelSpec spec() {
    fast::FixedShapeKernelSpec out;
    out.artifact_hash = 9'001;
    out.constraint_hash = 8'001;
    out.bundle_hash = 7'001;
    out.bundle_id = 17;
    out.leg_count = 2;
    out.asset_indices[0] = 100;
    out.asset_indices[1] = 101;
    out.market_indices[0] = 200;
    out.market_indices[1] = 201;
    out.sides[0] = oracle::Side::Buy;
    out.sides[1] = oracle::Side::Buy;
    out.ratio_qty_lots[0] = 1;
    out.ratio_qty_lots[1] = 1;
    out.guaranteed_payout_tick = 1'000'000;
    out.min_unit_edge_tick = 1;
    out.min_total_edge_tick = 1;
    out.min_edge_bps = 0;
    out.min_bundle_qty = 1;
    out.kernel_spec_hash = fast::hash_fixed_shape_kernel_spec(out);
    return out;
}

void FixedShapeKernelSpec_StandaloneHashStable() {
    const auto first = spec();
    const auto second = spec();

    expect_true(first.kernel_spec_hash != 0, "hash nonzero");
    expect_equal(first.kernel_spec_hash, second.kernel_spec_hash, "hash");
}

void FixedShapeKernelSpec_HashChangesWhenLegChanges() {
    const auto first = spec();
    auto second = spec();
    second.asset_indices[1] = 999;
    second.kernel_spec_hash = fast::hash_fixed_shape_kernel_spec(second);

    if (first.kernel_spec_hash == second.kernel_spec_hash) {
        fail("hash did not change");
    }
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"FixedShapeKernelSpec_StandaloneHashStable",
         &FixedShapeKernelSpec_StandaloneHashStable},
        {"FixedShapeKernelSpec_HashChangesWhenLegChanges",
         &FixedShapeKernelSpec_HashChangesWhenLegChanges},
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
