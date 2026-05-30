#pragma once

#include "oracle/compiler/CompiledComponent.h"
#include "oracle/compiler/ConstraintCompiler.h"
#include "oracle/enumerate/FeasibleStateOracle.h"

#include <string>
#include <unordered_map>

namespace trading_engine::oracle {

class SmallEnumOracle final : public FeasibleStateOracle {
public:
    SmallEnumOracle(
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
    CompiledConstraintSet compiled_;
    std::unordered_map<std::string, std::uint32_t> variable_index_;
};

}  // namespace trading_engine::oracle
