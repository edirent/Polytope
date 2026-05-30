#include "oracle/compiler/ComponentPartitioner.h"
#include "oracle/compiler/ConstraintCompiler.h"
#include "oracle/compiler/ConstraintGraph.h"
#include "oracle/enumerate/AtMostOneOracle.h"
#include "oracle/enumerate/ExactlyOneOracle.h"
#include "oracle/enumerate/FeasibleStateOracle.h"
#include "oracle/payoff/PayoutRule.h"
#include "oracle/rules/Rulebook.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using trading_engine::oracle::AtMostOneOracle;
using trading_engine::oracle::BooleanVariable;
using trading_engine::oracle::BundleObjectiveLeg;
using trading_engine::oracle::ComponentKind;
using trading_engine::oracle::ComponentPartitioner;
using trading_engine::oracle::CompiledConstraintSet;
using trading_engine::oracle::CompiledComponent;
using trading_engine::oracle::ConstraintCompiler;
using trading_engine::oracle::ConstraintGraphBuilder;
using trading_engine::oracle::ExactlyOneOracle;
using trading_engine::oracle::PAYOUT_ONE_TICK;
using trading_engine::oracle::RuleType;
using trading_engine::oracle::Rulebook;
using trading_engine::oracle::ValidatedRule;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
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
        out.push_back(BooleanVariable{
            .var_id = i,
            .variable_key = "m" + std::to_string(i) + ":Yes",
            .market_id = "m" + std::to_string(i),
            .outcome_id = "Yes",
            .asset_id = "asset_" + std::to_string(i)
        });
    }
    return out;
}

std::vector<std::string> variable_keys(std::uint32_t count) {
    std::vector<std::string> out;
    for (std::uint32_t i = 0; i < count; ++i) {
        out.push_back("m" + std::to_string(i) + ":Yes");
    }
    return out;
}

ValidatedRule rule(RuleType type, std::uint32_t count) {
    ValidatedRule out;
    out.rule_id = "r";
    out.type = type;
    out.variable_ids = variable_keys(count);
    out.approved = true;
    out.approved_by = "fixture";
    out.approved_at_ns = 1;
    return out;
}

struct CompiledFixture {
    CompiledConstraintSet compiled;
    CompiledComponent component;
};

CompiledFixture compile_fixture(RuleType type, std::uint32_t count) {
    Rulebook rulebook;
    rulebook.add_rule(rule(type, count));
    ConstraintCompiler compiler;
    auto compiled = compiler.compile(rulebook, variables(count)).compiled;

    ConstraintGraphBuilder graph_builder;
    ComponentPartitioner partitioner;
    const auto partition = partitioner.partition(
        graph_builder.build(compiled),
        compiled
    );

    return CompiledFixture{
        .compiled = std::move(compiled),
        .component = partition.components.front()
    };
}

std::vector<BundleObjectiveLeg> yes_objective(std::uint32_t count) {
    std::vector<BundleObjectiveLeg> objective;
    for (std::uint32_t i = 0; i < count; ++i) {
        objective.push_back(BundleObjectiveLeg{
            .variable_id = "m" + std::to_string(i) + ":Yes",
            .payout_if_true_tick = PAYOUT_ONE_TICK,
            .payout_if_false_tick = 0,
            .cost_tick = 0
        });
    }
    return objective;
}

std::vector<BundleObjectiveLeg> no_objective(std::uint32_t count) {
    std::vector<BundleObjectiveLeg> objective;
    for (std::uint32_t i = 0; i < count; ++i) {
        objective.push_back(BundleObjectiveLeg{
            .variable_id = "m" + std::to_string(i) + ":Yes",
            .payout_if_true_tick = 0,
            .payout_if_false_tick = PAYOUT_ONE_TICK,
            .cost_tick = 0
        });
    }
    return objective;
}

void ExactlyOneOracle_YesBasketMinPaysOne() {
    const auto fixture = compile_fixture(RuleType::ExactlyOne, 4);
    expect_equal(fixture.component.kind, ComponentKind::ExactlyOne, "kind");

    ExactlyOneOracle oracle(fixture.component, fixture.compiled);
    const auto result = oracle.min_payoff(yes_objective(4));
    expect_equal(result.min_profit_tick, PAYOUT_ONE_TICK, "min profit");
    expect_equal(result.is_guaranteed_positive, true, "positive");
}

void ExactlyOneOracle_NoBasketMinPaysNMinusOne() {
    const auto fixture = compile_fixture(RuleType::ExactlyOne, 4);
    ExactlyOneOracle oracle(fixture.component, fixture.compiled);

    const auto result = oracle.min_payoff(no_objective(4));
    expect_equal(result.min_profit_tick, 3 * PAYOUT_ONE_TICK, "min profit");
    expect_equal(result.is_guaranteed_positive, true, "positive");
}

void AtMostOneOracle_NoBasketMinPaysNMinusOne() {
    const auto fixture = compile_fixture(RuleType::AtMostOne, 4);
    expect_equal(fixture.component.kind, ComponentKind::AtMostOne, "kind");

    AtMostOneOracle oracle(fixture.component, fixture.compiled);
    const auto result = oracle.min_payoff(no_objective(4));
    expect_equal(result.min_profit_tick, 3 * PAYOUT_ONE_TICK, "min profit");
    expect_equal(result.is_guaranteed_positive, true, "positive");
}

void AtMostOneOracle_YesBasketCanAllLose() {
    const auto fixture = compile_fixture(RuleType::AtMostOne, 4);
    AtMostOneOracle oracle(fixture.component, fixture.compiled);

    const auto result = oracle.min_payoff(yes_objective(4));
    expect_equal(result.min_profit_tick, 0, "min profit");
    expect_equal(result.is_guaranteed_positive, false, "positive");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "ExactlyOneOracle_YesBasketMinPaysOne",
            &ExactlyOneOracle_YesBasketMinPaysOne
        },
        {
            "ExactlyOneOracle_NoBasketMinPaysNMinusOne",
            &ExactlyOneOracle_NoBasketMinPaysNMinusOne
        },
        {
            "AtMostOneOracle_NoBasketMinPaysNMinusOne",
            &AtMostOneOracle_NoBasketMinPaysNMinusOne
        },
        {
            "AtMostOneOracle_YesBasketCanAllLose",
            &AtMostOneOracle_YesBasketCanAllLose
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
