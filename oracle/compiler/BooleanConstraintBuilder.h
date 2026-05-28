#pragma once

#include "oracle/compiler/Constraint.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading_engine::oracle {

struct BooleanConstraintBuildResult {
    LinearBooleanConstraint constraint;
    std::vector<std::string> errors;

    [[nodiscard]] bool ok() const noexcept {
        return errors.empty();
    }
};

class BooleanConstraintBuilder {
public:
    using VariableIndex = std::unordered_map<std::string, std::uint32_t>;

    [[nodiscard]] BooleanConstraintBuildResult exactly_one(
        const std::vector<std::string>& variable_keys,
        const VariableIndex& variable_index
    ) const;

    [[nodiscard]] BooleanConstraintBuildResult at_most_one(
        const std::vector<std::string>& variable_keys,
        const VariableIndex& variable_index
    ) const;

    [[nodiscard]] BooleanConstraintBuildResult at_least_one(
        const std::vector<std::string>& variable_keys,
        const VariableIndex& variable_index
    ) const;

    [[nodiscard]] BooleanConstraintBuildResult implies(
        const std::vector<std::string>& variable_keys,
        const VariableIndex& variable_index
    ) const;

private:
    [[nodiscard]] BooleanConstraintBuildResult sum_constraint(
        const std::vector<std::string>& variable_keys,
        const VariableIndex& variable_index,
        ConstraintOp op,
        std::int32_t rhs
    ) const;
};

}  // namespace trading_engine::oracle
