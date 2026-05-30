#include "oracle/compiler/ComponentPartitioner.h"
#include "oracle/compiler/ConstraintCompiler.h"
#include "oracle/compiler/ConstraintGraph.h"
#include "oracle/compiler/MarketIntrinsicConstraintBuilder.h"
#include "oracle/ingestion/MarketUniverseBuilder.h"
#include "oracle/rules/Rulebook.h"

#include <algorithm>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using trading_engine::oracle::BooleanVariable;
using trading_engine::oracle::ComponentKind;
using trading_engine::oracle::ComponentPartitioner;
using trading_engine::oracle::ConstraintCompiler;
using trading_engine::oracle::ConstraintGraphBuilder;
using trading_engine::oracle::MarketIntrinsicConstraintBuilder;
using trading_engine::oracle::MarketUniverse;
using trading_engine::oracle::RawMarketRecord;
using trading_engine::oracle::RuleCoverage;
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

RawMarketRecord binary_market(std::string market_id) {
    RawMarketRecord out;
    out.market_id = std::move(market_id);
    out.event_id = "event";
    out.outcomes = {"Yes", "No"};
    out.asset_ids = {out.market_id + "_yes", out.market_id + "_no"};
    return out;
}

std::vector<BooleanVariable> binary_variables(
    const std::vector<RawMarketRecord>& markets
) {
    std::vector<BooleanVariable> out;
    for (const auto& market : markets) {
        for (std::size_t i = 0; i < market.outcomes.size(); ++i) {
            out.push_back(BooleanVariable{
                .var_id = static_cast<std::uint32_t>(out.size()),
                .variable_key = market.market_id + ":" + market.outcomes[i],
                .market_id = market.market_id,
                .outcome_id = market.outcomes[i],
                .asset_id = market.asset_ids[i]
            });
        }
    }
    return out;
}

ValidatedRule rule(
    const std::string& rule_id,
    RuleType type,
    const std::vector<std::string>& variable_ids,
    RuleCoverage coverage = RuleCoverage::ExclusiveOnly
) {
    ValidatedRule out;
    out.rule_id = rule_id;
    out.type = type;
    out.coverage = coverage;
    out.variable_ids = variable_ids;
    out.approved = true;
    out.approved_by = "fixture";
    out.approved_at_ns = 1;
    return out;
}

std::vector<std::string> variable_keys(std::uint32_t from, std::uint32_t count) {
    std::vector<std::string> out;
    for (std::uint32_t i = from; i < from + count; ++i) {
        out.push_back("m" + std::to_string(i) + ":Yes");
    }
    return out;
}

void ComponentPartitioner_AtMostOneClassifiedAsSemanticComponent() {
    Rulebook rulebook;
    rulebook.add_rule(rule("r", RuleType::AtMostOne, variable_keys(0, 40)));

    ConstraintCompiler compiler;
    const auto compiled = compiler.compile(rulebook, variables(40));
    ConstraintGraphBuilder graph_builder;
    ComponentPartitioner partitioner;
    const auto partition = partitioner.partition(
        graph_builder.build(compiled.compiled),
        compiled.compiled
    );

    expect_equal(partition.components.size(), 1U, "component count");
    expect_equal(
        partition.components.front().kind,
        ComponentKind::AtMostOne,
        "kind"
    );
    expect_equal(
        partition.components.front().variable_ids.size(),
        40U,
        "variable count"
    );
}

void ComponentPartitioner_SplitsIndependentRules() {
    Rulebook rulebook;
    rulebook.add_rule(rule("r1", RuleType::ExactlyOne, variable_keys(0, 3)));
    rulebook.add_rule(rule("r2", RuleType::ExactlyOne, variable_keys(3, 2)));

    ConstraintCompiler compiler;
    const auto compiled = compiler.compile(rulebook, variables(5));
    ConstraintGraphBuilder graph_builder;
    ComponentPartitioner partitioner;
    const auto partition = partitioner.partition(
        graph_builder.build(compiled.compiled),
        compiled.compiled
    );

    expect_equal(partition.components.size(), 2U, "component count");
    expect_equal(partition.components[0].kind, ComponentKind::ExactlyOne, "kind0");
    expect_equal(partition.components[1].kind, ComponentKind::ExactlyOne, "kind1");
    expect_equal(partition.partition_hash != 0, true, "partition hash");
}

void ComponentPartitioner_ExhaustiveMutualExclusionWithComplementsIsExactlyOne() {
    MarketUniverse universe;
    for (std::uint32_t i = 0; i < 48; ++i) {
        universe.markets.push_back(binary_market("world_cup_" + std::to_string(i)));
    }
    const auto vars = binary_variables(universe.markets);

    std::vector<std::string> yes_variables;
    for (std::uint32_t i = 0; i < 48; ++i) {
        yes_variables.push_back("world_cup_" + std::to_string(i) + ":Yes");
    }

    Rulebook rulebook;
    rulebook.add_rule(rule(
        "world_cup_winner",
        RuleType::MutuallyExclusive,
        yes_variables,
        RuleCoverage::ExhaustiveAndExclusive
    ));

    ConstraintCompiler compiler;
    auto compiled = compiler.compile(rulebook, vars).compiled;
    MarketIntrinsicConstraintBuilder intrinsic_builder;
    const auto intrinsic = intrinsic_builder.build(universe, compiled.variables);
    compiled.constraints.insert(
        compiled.constraints.end(),
        intrinsic.begin(),
        intrinsic.end()
    );

    ConstraintGraphBuilder graph_builder;
    ComponentPartitioner partitioner;
    const auto partition = partitioner.partition(
        graph_builder.build(compiled),
        compiled
    );

    std::uint32_t exactly_one_components = 0;
    std::uint32_t max_component_variables = 0;
    for (const auto& component : partition.components) {
        if (component.kind == ComponentKind::ExactlyOne) {
            ++exactly_one_components;
        }
        max_component_variables = std::max<std::uint32_t>(
            max_component_variables,
            static_cast<std::uint32_t>(component.variable_ids.size())
        );
    }

    expect_equal(exactly_one_components, 1U, "exactly one components");
    expect_equal(max_component_variables, 96U, "max component variables");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "ComponentPartitioner_AtMostOneClassifiedAsSemanticComponent",
            &ComponentPartitioner_AtMostOneClassifiedAsSemanticComponent
        },
        {
            "ComponentPartitioner_SplitsIndependentRules",
            &ComponentPartitioner_SplitsIndependentRules
        },
        {
            "ComponentPartitioner_ExhaustiveMutualExclusionWithComplementsIsExactlyOne",
            &ComponentPartitioner_ExhaustiveMutualExclusionWithComplementsIsExactlyOne
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
