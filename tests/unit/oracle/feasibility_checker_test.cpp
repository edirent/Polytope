#include "oracle/enumerate/FeasibilityChecker.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::oracle::BooleanVariable;
using trading_engine::oracle::CompiledConstraintSet;
using trading_engine::oracle::ConstraintOp;
using trading_engine::oracle::FeasibilityChecker;
using trading_engine::oracle::LinearBooleanConstraint;
using trading_engine::oracle::state_bitset_from_mask;

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

CompiledConstraintSet exactly_one_two_vars() {
    CompiledConstraintSet compiled;
    compiled.variables = {
        BooleanVariable{0, "m1:YES", "m1", "YES", "asset_yes"},
        BooleanVariable{1, "m1:NO", "m1", "NO", "asset_no"}
    };
    compiled.constraints.push_back(
        LinearBooleanConstraint{
            .var_ids = {0, 1},
            .coeffs = {1, 1},
            .op = ConstraintOp::Equal,
            .rhs = 1
        }
    );
    return compiled;
}

void FeasibilityChecker_SatisfiesExactlyOne() {
    FeasibilityChecker checker;
    const auto result = checker.check(
        exactly_one_two_vars(),
        state_bitset_from_mask(0b01)
    );

    expect_true(result.ok(), "check ok");
    expect_true(result.feasible, "feasible");
}

void FeasibilityChecker_DetectsContradiction() {
    FeasibilityChecker checker;
    const auto result = checker.check(
        exactly_one_two_vars(),
        state_bitset_from_mask(0b11)
    );

    expect_true(result.ok(), "check ok");
    expect_false(result.feasible, "feasible");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "FeasibilityChecker_SatisfiesExactlyOne",
            &FeasibilityChecker_SatisfiesExactlyOne
        },
        {
            "FeasibilityChecker_DetectsContradiction",
            &FeasibilityChecker_DetectsContradiction
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
