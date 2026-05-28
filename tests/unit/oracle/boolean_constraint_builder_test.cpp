#include "oracle/compiler/BooleanConstraintBuilder.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::oracle::BooleanConstraintBuilder;
using trading_engine::oracle::ConstraintOp;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

void expect_equal(
    std::uint32_t actual,
    std::uint32_t expected,
    const std::string& field
) {
    if (actual != expected) {
        fail("mismatch: " + field);
    }
}

void expect_equal(
    std::int32_t actual,
    std::int32_t expected,
    const std::string& field
) {
    if (actual != expected) {
        fail("mismatch: " + field);
    }
}

void expect_equal(
    ConstraintOp actual,
    ConstraintOp expected,
    const std::string& field
) {
    if (actual != expected) {
        fail("mismatch: " + field);
    }
}

BooleanConstraintBuilder::VariableIndex index() {
    return {
        {"m1:YES", 0},
        {"m1:NO", 1},
        {"m2:YES", 2}
    };
}

void BooleanConstraintBuilder_ExactlyOne() {
    BooleanConstraintBuilder builder;
    const auto result = builder.exactly_one({"m1:YES", "m1:NO"}, index());

    expect_true(result.ok(), "build ok");
    expect_equal(result.constraint.op, ConstraintOp::Equal, "op");
    expect_equal(result.constraint.rhs, 1, "rhs");
    expect_equal(result.constraint.var_ids.size(), 2U, "var count");
    expect_equal(result.constraint.coeffs[0], 1, "coeff 0");
    expect_equal(result.constraint.coeffs[1], 1, "coeff 1");
}

void BooleanConstraintBuilder_AtMostOne() {
    BooleanConstraintBuilder builder;
    const auto result = builder.at_most_one(
        {"m1:YES", "m1:NO", "m2:YES"},
        index()
    );

    expect_true(result.ok(), "build ok");
    expect_equal(result.constraint.op, ConstraintOp::LessEqual, "op");
    expect_equal(result.constraint.rhs, 1, "rhs");
    expect_equal(result.constraint.var_ids.size(), 3U, "var count");
}

void BooleanConstraintBuilder_AtLeastOne() {
    BooleanConstraintBuilder builder;
    const auto result = builder.at_least_one(
        {"m1:YES", "m1:NO", "m2:YES"},
        index()
    );

    expect_true(result.ok(), "build ok");
    expect_equal(result.constraint.op, ConstraintOp::GreaterEqual, "op");
    expect_equal(result.constraint.rhs, 1, "rhs");
    expect_equal(result.constraint.var_ids.size(), 3U, "var count");
}

void BooleanConstraintBuilder_Implies() {
    BooleanConstraintBuilder builder;
    const auto result = builder.implies({"m1:YES", "m1:NO"}, index());

    expect_true(result.ok(), "build ok");
    expect_equal(result.constraint.op, ConstraintOp::LessEqual, "op");
    expect_equal(result.constraint.rhs, 0, "rhs");
    expect_equal(result.constraint.var_ids[0], 0U, "var 0");
    expect_equal(result.constraint.var_ids[1], 1U, "var 1");
    expect_equal(result.constraint.coeffs[0], 1, "coeff 0");
    expect_equal(result.constraint.coeffs[1], -1, "coeff 1");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "BooleanConstraintBuilder_ExactlyOne",
            &BooleanConstraintBuilder_ExactlyOne
        },
        {
            "BooleanConstraintBuilder_AtMostOne",
            &BooleanConstraintBuilder_AtMostOne
        },
        {
            "BooleanConstraintBuilder_AtLeastOne",
            &BooleanConstraintBuilder_AtLeastOne
        },
        {"BooleanConstraintBuilder_Implies", &BooleanConstraintBuilder_Implies}
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
