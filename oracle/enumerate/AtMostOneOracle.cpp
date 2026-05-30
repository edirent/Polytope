#include "oracle/enumerate/AtMostOneOracle.h"

#include <limits>

namespace trading_engine::oracle {

AtMostOneOracle::AtMostOneOracle(
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

ComponentId AtMostOneOracle::component_id() const {
    return component_id_;
}

WorstCaseResult AtMostOneOracle::min_payoff(
    const std::vector<BundleObjectiveLeg>& objective
) const {
    WorstCaseResult result;
    result.component_id = component_id_;
    result.min_profit_tick = std::numeric_limits<std::int64_t>::max();

    auto evaluate = [&](const std::string* true_variable) {
        std::int64_t profit = 0;
        for (const auto& leg : objective) {
            const bool is_true =
                true_variable != nullptr && leg.variable_id == *true_variable;
            profit += is_true ? leg.payout_if_true_tick
                              : leg.payout_if_false_tick;
            profit -= leg.cost_tick;
        }
        if (profit < result.min_profit_tick) {
            result.min_profit_tick = profit;
            result.witness_true_variables =
                true_variable == nullptr ? std::vector<std::string>{}
                                         : std::vector<std::string>{*true_variable};
        }
    };

    evaluate(nullptr);
    for (const auto& variable_id : variable_ids_) {
        evaluate(&variable_id);
    }

    if (result.min_profit_tick == std::numeric_limits<std::int64_t>::max()) {
        result.min_profit_tick = 0;
    }
    result.is_guaranteed_positive = result.min_profit_tick > 0;
    return result;
}

bool AtMostOneOracle::is_feasible(const StateBitset& state) const {
    std::uint32_t true_count = 0;
    for (const auto var_id : var_ids_) {
        if (state.test(var_id)) {
            ++true_count;
        }
    }
    return true_count <= 1;
}

}  // namespace trading_engine::oracle
