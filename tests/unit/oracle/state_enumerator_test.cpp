#include "oracle/enumerate/StateEnumerator.h"

#include "oracle/compiler/ConstraintCompiler.h"
#include "oracle/rules/Rulebook.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

using trading_engine::oracle::BooleanVariable;
using trading_engine::oracle::CompiledConstraintSet;
using trading_engine::oracle::ConstraintOp;
using trading_engine::oracle::ConstraintCompiler;
using trading_engine::oracle::LinearBooleanConstraint;
using trading_engine::oracle::OracleErrorCode;
using trading_engine::oracle::RuleType;
using trading_engine::oracle::Rulebook;
using trading_engine::oracle::StateEnumerator;
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

std::vector<BooleanVariable> variables(std::uint32_t count) {
    std::vector<BooleanVariable> out;
    for (std::uint32_t i = 0; i < count; ++i) {
        BooleanVariable variable;
        variable.var_id = i;
        variable.variable_key = "m1:" + std::to_string(i);
        variable.market_id = "m1";
        variable.outcome_id = std::to_string(i);
        variable.asset_id = "asset_" + std::to_string(i);
        out.push_back(std::move(variable));
    }
    return out;
}

CompiledConstraintSet sum_constraint(
    std::uint32_t variable_count,
    ConstraintOp op,
    std::int32_t rhs
) {
    CompiledConstraintSet compiled;
    compiled.variables = variables(variable_count);

    LinearBooleanConstraint constraint;
    for (std::uint32_t i = 0; i < variable_count; ++i) {
        constraint.var_ids.push_back(i);
        constraint.coeffs.push_back(1);
    }
    constraint.op = op;
    constraint.rhs = rhs;
    compiled.constraints.push_back(std::move(constraint));
    return compiled;
}

CompiledConstraintSet contradiction() {
    CompiledConstraintSet compiled = sum_constraint(1, ConstraintOp::Equal, 1);
    compiled.constraints.push_back(
        LinearBooleanConstraint{
            .var_ids = {0},
            .coeffs = {1},
            .op = ConstraintOp::Equal,
            .rhs = 0
        }
    );
    return compiled;
}

std::vector<BooleanVariable> fixture_variables() {
    std::vector<BooleanVariable> out;
    out.push_back(BooleanVariable{
        .var_id = 0,
        .variable_key = "m1:YES",
        .market_id = "m1",
        .outcome_id = "YES",
        .asset_id = "asset_yes"
    });
    out.push_back(BooleanVariable{
        .var_id = 1,
        .variable_key = "m1:NO",
        .market_id = "m1",
        .outcome_id = "NO",
        .asset_id = "asset_no"
    });
    return out;
}

ValidatedRule validated_rule(
    std::string rule_id,
    RuleType type,
    std::vector<std::string> variable_ids
) {
    ValidatedRule rule;
    rule.rule_id = std::move(rule_id);
    rule.type = type;
    rule.variable_ids = std::move(variable_ids);
    rule.approved = true;
    rule.approved_by = "fixture";
    rule.approved_at_ns = 1;
    rule.source_rule_draft_id = "draft";
    return rule;
}

void StateEnumerator_ExactlyOneTwoVarsProducesTwoStates() {
    StateEnumerator enumerator;
    const auto result = enumerator.enumerate(
        sum_constraint(2, ConstraintOp::Equal, 1)
    );

    expect_true(result.ok(), "enumerate ok");
    expect_equal(result.feasible_states.size(), 2U, "state count");
    expect_equal(result.feasible_states[0].state_id, 1ULL, "state 0");
    expect_equal(result.feasible_states[1].state_id, 2ULL, "state 1");
}

void StateEnumerator_ExactlyOneThreeVarsProducesThreeStates() {
    StateEnumerator enumerator;
    const auto result = enumerator.enumerate(
        sum_constraint(3, ConstraintOp::Equal, 1)
    );

    expect_true(result.ok(), "enumerate ok");
    expect_equal(result.feasible_states.size(), 3U, "state count");
    expect_equal(result.feasible_states[0].state_id, 1ULL, "state 0");
    expect_equal(result.feasible_states[1].state_id, 2ULL, "state 1");
    expect_equal(result.feasible_states[2].state_id, 4ULL, "state 2");
}

void StateEnumerator_AtMostOneThreeVarsProducesFourStates() {
    StateEnumerator enumerator;
    const auto result = enumerator.enumerate(
        sum_constraint(3, ConstraintOp::LessEqual, 1)
    );

    expect_true(result.ok(), "enumerate ok");
    expect_equal(result.feasible_states.size(), 4U, "state count");
    expect_equal(result.feasible_states[0].state_id, 0ULL, "state 0");
    expect_equal(result.feasible_states[1].state_id, 1ULL, "state 1");
    expect_equal(result.feasible_states[2].state_id, 2ULL, "state 2");
    expect_equal(result.feasible_states[3].state_id, 4ULL, "state 3");
}

void StateEnumerator_RejectsMoreThan32Vars() {
    StateEnumerator enumerator;
    CompiledConstraintSet compiled;
    compiled.variables = variables(33);

    const auto result = enumerator.enumerate(compiled);

    expect_false(result.ok(), "enumerate ok");
    expect_equal(result.code, OracleErrorCode::TooManyVariables, "code");
    expect_true(result.feasible_states.empty(), "states empty");
}

void StateEnumerator_DeterministicStateOrdering() {
    StateEnumerator enumerator;
    const auto compiled = sum_constraint(3, ConstraintOp::LessEqual, 1);

    const auto a = enumerator.enumerate(compiled);
    const auto b = enumerator.enumerate(compiled);

    expect_true(a.ok(), "enumerate a ok");
    expect_true(b.ok(), "enumerate b ok");
    expect_equal(a.feasible_states.size(), b.feasible_states.size(), "count");
    for (std::size_t i = 0; i < a.feasible_states.size(); ++i) {
        expect_equal(
            a.feasible_states[i].state_id,
            b.feasible_states[i].state_id,
            "state id"
        );
    }
}

void StateEnumerator_ContradictoryRulebookProducesZeroStates() {
    StateEnumerator enumerator;
    const auto result = enumerator.enumerate(contradiction());

    expect_true(result.ok(), "enumerate ok");
    expect_equal(result.feasible_states.size(), 0U, "state count");
}

void StateEnumerator_ValidFixtureProducesFeasibleStates() {
    const auto fixture_path =
        std::filesystem::path{POLYTOPE_SOURCE_DIR} /
        "tests/fixtures/oracle/rulebook_small.json";
    const auto loaded = Rulebook::load_json(fixture_path.string());
    expect_true(loaded.ok(), "load fixture");

    Rulebook rulebook;
    for (const auto& rule : loaded.rules) {
        rulebook.add_rule(rule);
    }

    ConstraintCompiler compiler;
    const auto compiled = compiler.compile(rulebook, fixture_variables());
    expect_true(compiled.ok(), "compile fixture");
    expect_true(!compiled.compiled.constraints.empty(), "constraints");

    StateEnumerator enumerator;
    const auto result = enumerator.enumerate(compiled.compiled);
    expect_true(result.ok(), "enumerate fixture");
    expect_equal(result.feasible_states.size(), 2U, "feasible count");
    expect_equal(result.feasible_states[0].state_id, 1ULL, "state 0");
    expect_equal(result.feasible_states[1].state_id, 2ULL, "state 1");
}

void StateEnumerator_ContradictoryCompiledRulebookProducesZeroStates() {
    Rulebook rulebook;
    rulebook.add_rule(validated_rule(
        "exactly_one",
        RuleType::ExactlyOne,
        {"m1:YES", "m1:NO"}
    ));
    rulebook.add_rule(validated_rule(
        "yes_implies_no",
        RuleType::Implies,
        {"m1:YES", "m1:NO"}
    ));
    rulebook.add_rule(validated_rule(
        "no_implies_yes",
        RuleType::Implies,
        {"m1:NO", "m1:YES"}
    ));

    ConstraintCompiler compiler;
    const auto compiled = compiler.compile(rulebook, fixture_variables());
    expect_true(compiled.ok(), "compile contradictory rulebook");

    StateEnumerator enumerator;
    const auto result = enumerator.enumerate(compiled.compiled);
    expect_true(result.ok(), "enumerate contradictory rulebook");
    expect_equal(result.feasible_states.size(), 0U, "state count");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "StateEnumerator_ExactlyOneTwoVarsProducesTwoStates",
            &StateEnumerator_ExactlyOneTwoVarsProducesTwoStates
        },
        {
            "StateEnumerator_ExactlyOneThreeVarsProducesThreeStates",
            &StateEnumerator_ExactlyOneThreeVarsProducesThreeStates
        },
        {
            "StateEnumerator_AtMostOneThreeVarsProducesFourStates",
            &StateEnumerator_AtMostOneThreeVarsProducesFourStates
        },
        {
            "StateEnumerator_RejectsMoreThan32Vars",
            &StateEnumerator_RejectsMoreThan32Vars
        },
        {
            "StateEnumerator_DeterministicStateOrdering",
            &StateEnumerator_DeterministicStateOrdering
        },
        {
            "StateEnumerator_ContradictoryRulebookProducesZeroStates",
            &StateEnumerator_ContradictoryRulebookProducesZeroStates
        },
        {
            "StateEnumerator_ValidFixtureProducesFeasibleStates",
            &StateEnumerator_ValidFixtureProducesFeasibleStates
        },
        {
            "StateEnumerator_ContradictoryCompiledRulebookProducesZeroStates",
            &StateEnumerator_ContradictoryCompiledRulebookProducesZeroStates
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
