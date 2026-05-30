#pragma once

#include "oracle/compiler/CompiledComponent.h"
#include "oracle/compiler/ConstraintCompiler.h"
#include "oracle/enumerate/FeasibleStateOracle.h"

#include <string>
#include <vector>

namespace trading_engine::oracle {

class AtMostOneOracle final : public FeasibleStateOracle {
public:
    AtMostOneOracle(
        const CompiledComponent& component,
        const CompiledConstraintSet& compiled
    );

    [[nodiscard]] ComponentId component_id() const override;

    [[nodiscard]] WorstCaseResult min_payoff(
        const std::vector<BundleObjectiveLeg>& objective
    ) const override;

    [[nodiscard]] bool is_feasible(const StateBitset& state) const override;

private:
    ComponentId component_id_ = 0;
    std::vector<std::uint32_t> var_ids_;
    std::vector<std::string> variable_ids_;
};

}  // namespace trading_engine::oracle
