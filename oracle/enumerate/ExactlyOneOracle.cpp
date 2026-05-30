#include "oracle/enumerate/ExactlyOneOracle.h"

#include <limits>

namespace trading_engine::oracle {

ExactlyOneOracle::ExactlyOneOracle(
    const CompiledComponent& component,
    const CompiledConstraintSet& compiled
)
    : component_id_(component.component_id),
      var_ids_(component.variable_ids) {
    variable_ids_.reserve(component.variable_ids.size());
    for (const auto var_id : component.variable_ids) {
        variable_ids_.push_back(compiled.variables[var_id].variable_key);
    }
}

ComponentId ExactlyOneOracle::component_id() const {
    return component_id_;
}

WorstCaseResult ExactlyOneOracle::min_payoff(
    const std::vector<BundleObjectiveLeg>& objective
) const {
    WorstCaseResult result;
    result.component_id = component_id_;
    result.min_profit_tick = std::numeric_limits<std::int64_t>::max();

    for (const auto& true_variable : variable_ids_) {
        std::int64_t profit = 0;
        for (const auto& leg : objective) {
            profit += leg.variable_id == true_variable
                          ? leg.payout_if_true_tick
                          : leg.payout_if_false_tick;
            profit -= leg.cost_tick;
        }
        if (profit < result.min_profit_tick) {
            result.min_profit_tick = profit;
            result.witness_true_variables = {true_variable};
        }
    }

    if (result.min_profit_tick == std::numeric_limits<std::int64_t>::max()) {
        result.min_profit_tick = 0;
    }
    result.is_guaranteed_positive = result.min_profit_tick > 0;
    return result;
}

bool ExactlyOneOracle::is_feasible(const StateBitset& state) const {
    std::uint32_t true_count = 0;
    for (const auto var_id : var_ids_) {
        if (state.test(var_id)) {
            ++true_count;
        }
    }
    return true_count == 1;
}

}  // namespace trading_engine::oracle
