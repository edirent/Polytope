#include "oracle/enumerate/SmallEnumOracle.h"

#include "oracle/enumerate/FeasibilityChecker.h"
#include "oracle/enumerate/StateEnumerator.h"

#include <limits>
#include <unordered_map>

namespace trading_engine::oracle {
namespace {

[[nodiscard]] CompiledConstraintSet subset_compiled_set(
    const CompiledComponent& component,
    const CompiledConstraintSet& compiled
) {
    CompiledConstraintSet out;
    std::unordered_map<std::uint32_t, std::uint32_t> old_to_new;
    for (std::size_t i = 0; i < component.variable_ids.size(); ++i) {
        const auto old_var_id = component.variable_ids[i];
        auto variable = compiled.variables[old_var_id];
        variable.var_id = static_cast<std::uint32_t>(i);
        old_to_new.emplace(old_var_id, variable.var_id);
        out.variables.push_back(std::move(variable));
    }
    for (const auto constraint_id : component.constraint_ids) {
        auto constraint = compiled.constraints[constraint_id];
        for (auto& var_id : constraint.var_ids) {
            var_id = old_to_new.at(var_id);
        }
        out.constraints.push_back(std::move(constraint));
    }
    out.constraint_hash = hash_compiled_constraints(out);
    return out;
}

}  // namespace

SmallEnumOracle::SmallEnumOracle(
    const CompiledComponent& component,
    const CompiledConstraintSet& compiled
)
    : component_id_(component.component_id),
      compiled_(subset_compiled_set(component, compiled)) {
    for (const auto& variable : compiled_.variables) {
        variable_index_.emplace(variable.variable_key, variable.var_id);
    }
}

ComponentId SmallEnumOracle::component_id() const {
    return component_id_;
}

WorstCaseResult SmallEnumOracle::min_payoff(
    const std::vector<BundleObjectiveLeg>& objective
) const {
    WorstCaseResult result;
    result.component_id = component_id_;
    result.min_profit_tick = std::numeric_limits<std::int64_t>::max();

    StateEnumerator enumerator;
    const auto enumeration = enumerator.enumerate(compiled_);
    for (const auto& feasible : enumeration.feasible_states) {
        StateBitset state{feasible.bitset_words};
        std::int64_t profit = 0;
        std::vector<std::string> witness;
        for (const auto& leg : objective) {
            const auto it = variable_index_.find(leg.variable_id);
            if (it == variable_index_.end()) {
                continue;
            }
            const bool value = state.test(it->second);
            profit += value ? leg.payout_if_true_tick
                            : leg.payout_if_false_tick;
            profit -= leg.cost_tick;
            if (value) {
                witness.push_back(leg.variable_id);
            }
        }
        if (profit < result.min_profit_tick) {
            result.min_profit_tick = profit;
            result.witness_true_variables = std::move(witness);
        }
    }

    if (result.min_profit_tick == std::numeric_limits<std::int64_t>::max()) {
        result.min_profit_tick = 0;
    }
    result.is_guaranteed_positive = result.min_profit_tick > 0;
    return result;
}

bool SmallEnumOracle::is_feasible(const StateBitset& state) const {
    FeasibilityChecker checker;
    return checker.check(compiled_, state).feasible;
}

}  // namespace trading_engine::oracle
