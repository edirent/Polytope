#include "oracle/compiler/ConstraintCompiler.h"
#include "oracle/compiler/MatrixBuilder.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

using trading_engine::oracle::BooleanVariable;
using trading_engine::oracle::ConstraintCompiler;
using trading_engine::oracle::ConstraintOp;
using trading_engine::oracle::MatrixBuilder;
using trading_engine::oracle::RuleType;
using trading_engine::oracle::Rulebook;
using trading_engine::oracle::ValidatedRule;

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

BooleanVariable variable(
    std::string key,
    std::string outcome,
    std::string asset
) {
    BooleanVariable out;
    out.variable_key = std::move(key);
    out.market_id = "m1";
    out.outcome_id = std::move(outcome);
    out.asset_id = std::move(asset);
    return out;
}

std::vector<BooleanVariable> variables() {
    return {
        variable("m1:YES", "YES", "asset_yes"),
        variable("m1:NO", "NO", "asset_no"),
        variable("m1:MAYBE", "MAYBE", "asset_maybe")
    };
}

ValidatedRule rule(
    std::string rule_id,
    RuleType type,
    std::vector<std::string> variable_ids,
    bool approved = true
) {
    ValidatedRule out;
    out.rule_id = std::move(rule_id);
    out.type = type;
    out.variable_ids = std::move(variable_ids);
    out.approved = approved;
    out.approved_by = approved ? "fixture" : "";
    out.approved_at_ns = approved ? 1 : 0;
    out.source_rule_draft_id = "draft";
    return out;
}

Rulebook rulebook_with(const ValidatedRule& value) {
    Rulebook out;
    out.add_rule(value);
    return out;
}

void ConstraintCompiler_ExactlyOne() {
    ConstraintCompiler compiler;
    const auto result = compiler.compile(
        rulebook_with(rule(
            "r1",
            RuleType::ExactlyOne,
            {"m1:YES", "m1:NO"}
        )),
        variables()
    );

    expect_true(result.ok(), "compile ok");
    expect_equal(result.compiled.constraints.size(), 1U, "constraint count");
    const auto& constraint = result.compiled.constraints.front();
    expect_equal(constraint.op, ConstraintOp::Equal, "op");
    expect_equal(constraint.rhs, 1, "rhs");
    expect_equal(constraint.coeffs.size(), 2U, "coeff count");
}

void ConstraintCompiler_AtMostOne() {
    ConstraintCompiler compiler;
    const auto result = compiler.compile(
        rulebook_with(rule(
            "r1",
            RuleType::AtMostOne,
            {"m1:YES", "m1:NO", "m1:MAYBE"}
        )),
        variables()
    );

    expect_true(result.ok(), "compile ok");
    const auto& constraint = result.compiled.constraints.front();
    expect_equal(constraint.op, ConstraintOp::LessEqual, "op");
    expect_equal(constraint.rhs, 1, "rhs");
    expect_equal(constraint.var_ids.size(), 3U, "var count");
}

void ConstraintCompiler_AtLeastOne() {
    ConstraintCompiler compiler;
    const auto result = compiler.compile(
        rulebook_with(rule(
            "r1",
            RuleType::AtLeastOne,
            {"m1:YES", "m1:NO", "m1:MAYBE"}
        )),
        variables()
    );

    expect_true(result.ok(), "compile ok");
    const auto& constraint = result.compiled.constraints.front();
    expect_equal(constraint.op, ConstraintOp::GreaterEqual, "op");
    expect_equal(constraint.rhs, 1, "rhs");
    expect_equal(constraint.var_ids.size(), 3U, "var count");
}

void ConstraintCompiler_Implies() {
    ConstraintCompiler compiler;
    const auto result = compiler.compile(
        rulebook_with(rule(
            "r1",
            RuleType::Implies,
            {"m1:YES", "m1:NO"}
        )),
        variables()
    );

    expect_true(result.ok(), "compile ok");
    const auto& constraint = result.compiled.constraints.front();
    expect_equal(constraint.op, ConstraintOp::LessEqual, "op");
    expect_equal(constraint.rhs, 0, "rhs");
    expect_equal(constraint.coeffs[0], 1, "coeff 0");
    expect_equal(constraint.coeffs[1], -1, "coeff 1");
}

void ConstraintCompiler_RejectsUnapprovedRules() {
    ConstraintCompiler compiler;
    const auto result = compiler.compile(
        rulebook_with(rule(
            "r1",
            RuleType::ExactlyOne,
            {"m1:YES", "m1:NO"},
            false
        )),
        variables()
    );

    expect_false(result.ok(), "compile ok");
    expect_false(result.errors.empty(), "errors");
    expect_equal(result.compiled.constraints.size(), 0U, "constraint count");
}

void ConstraintCompiler_RejectsUnknownVariable() {
    ConstraintCompiler compiler;
    const auto result = compiler.compile(
        rulebook_with(rule(
            "r1",
            RuleType::ExactlyOne,
            {"m1:YES", "m1:UNKNOWN"}
        )),
        variables()
    );

    expect_false(result.ok(), "compile ok");
    expect_false(result.errors.empty(), "errors");
    expect_equal(result.compiled.constraints.size(), 0U, "constraint count");
}

void ConstraintCompiler_DeterministicVariableOrdering() {
    ConstraintCompiler compiler;
    auto shuffled = variables();
    std::swap(shuffled[0], shuffled[2]);

    const auto result = compiler.compile(
        rulebook_with(rule(
            "r1",
            RuleType::ExactlyOne,
            {"m1:YES", "m1:NO"}
        )),
        shuffled
    );

    expect_true(result.ok(), "compile ok");
    expect_equal(
        result.compiled.variables[0].variable_key,
        std::string{"m1:MAYBE"},
        "variable 0"
    );
    expect_equal(
        result.compiled.variables[1].variable_key,
        std::string{"m1:NO"},
        "variable 1"
    );
    expect_equal(
        result.compiled.variables[2].variable_key,
        std::string{"m1:YES"},
        "variable 2"
    );
    expect_equal(result.compiled.variables[0].var_id, 0U, "var_id 0");
    expect_equal(result.compiled.variables[1].var_id, 1U, "var_id 1");
    expect_equal(result.compiled.variables[2].var_id, 2U, "var_id 2");
}

void ConstraintCompiler_DeterministicConstraintHash() {
    ConstraintCompiler compiler;
    auto shuffled = variables();
    std::swap(shuffled[0], shuffled[2]);

    Rulebook rules;
    rules.add_rule(rule(
        "r1",
        RuleType::ExactlyOne,
        {"m1:YES", "m1:NO"}
    ));
    rules.add_rule(rule(
        "r2",
        RuleType::Implies,
        {"m1:YES", "m1:NO"}
    ));

    const auto a = compiler.compile(rules, variables());
    const auto b = compiler.compile(rules, shuffled);

    expect_true(a.ok(), "compile a ok");
    expect_true(b.ok(), "compile b ok");
    expect_equal(
        a.compiled.constraint_hash,
        b.compiled.constraint_hash,
        "constraint hash"
    );
    expect_true(a.compiled.constraint_hash != 0, "constraint hash nonzero");
}

void ConstraintCompiler_WritesMatrixArtifacts() {
    ConstraintCompiler compiler;
    const auto result = compiler.compile(
        rulebook_with(rule(
            "r1",
            RuleType::ExactlyOne,
            {"m1:YES", "m1:NO"}
        )),
        variables()
    );
    expect_true(result.ok(), "compile ok");

    const auto out_dir =
        std::filesystem::temp_directory_path() /
        "oracle_constraint_compiler_artifacts";
    std::filesystem::remove_all(out_dir);

    MatrixBuilder writer;
    std::vector<std::string> errors;
    expect_true(
        writer.write(result.compiled, out_dir.string(), &errors),
        "write matrix"
    );
    expect_true(errors.empty(), "write errors");
    expect_true(
        std::filesystem::exists(out_dir / "variables.bin"),
        "variables.bin"
    );
    expect_true(
        std::filesystem::exists(out_dir / "constraints.bin"),
        "constraints.bin"
    );
    expect_true(
        std::filesystem::exists(out_dir / "variables.debug.json"),
        "variables.debug.json"
    );
    expect_true(
        std::filesystem::exists(out_dir / "constraints.debug.json"),
        "constraints.debug.json"
    );

    std::filesystem::remove_all(out_dir);
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"ConstraintCompiler_ExactlyOne", &ConstraintCompiler_ExactlyOne},
        {"ConstraintCompiler_AtMostOne", &ConstraintCompiler_AtMostOne},
        {"ConstraintCompiler_AtLeastOne", &ConstraintCompiler_AtLeastOne},
        {"ConstraintCompiler_Implies", &ConstraintCompiler_Implies},
        {
            "ConstraintCompiler_RejectsUnapprovedRules",
            &ConstraintCompiler_RejectsUnapprovedRules
        },
        {
            "ConstraintCompiler_RejectsUnknownVariable",
            &ConstraintCompiler_RejectsUnknownVariable
        },
        {
            "ConstraintCompiler_DeterministicVariableOrdering",
            &ConstraintCompiler_DeterministicVariableOrdering
        },
        {
            "ConstraintCompiler_DeterministicConstraintHash",
            &ConstraintCompiler_DeterministicConstraintHash
        },
        {
            "ConstraintCompiler_WritesMatrixArtifacts",
            &ConstraintCompiler_WritesMatrixArtifacts
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
