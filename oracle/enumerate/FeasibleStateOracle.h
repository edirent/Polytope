#pragma once

#include "oracle/compiler/CompiledComponent.h"
#include "oracle/enumerate/StateBitset.h"

#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::oracle {

struct BundleObjectiveLeg {
    std::string variable_id;
    std::int64_t payout_if_true_tick = 0;
    std::int64_t payout_if_false_tick = 0;
    std::int64_t cost_tick = 0;
};

struct WorstCaseResult {
    std::int64_t min_profit_tick = 0;
    bool is_guaranteed_positive = false;
    std::vector<std::string> witness_true_variables;
    ComponentId component_id = 0;
};

class FeasibleStateOracle {
public:
    virtual ~FeasibleStateOracle() = default;

    [[nodiscard]] virtual ComponentId component_id() const = 0;

    [[nodiscard]] virtual WorstCaseResult min_payoff(
        const std::vector<BundleObjectiveLeg>& objective
    ) const = 0;

    [[nodiscard]] virtual bool is_feasible(
        const StateBitset& state
    ) const = 0;
};

}  // namespace trading_engine::oracle
