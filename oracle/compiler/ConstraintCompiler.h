#pragma once

#include "oracle/compiler/BooleanVariable.h"
#include "oracle/compiler/Constraint.h"
#include "oracle/rules/Rulebook.h"

#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::oracle {

struct CompiledConstraintSet {
    std::vector<BooleanVariable> variables;
    std::vector<LinearBooleanConstraint> constraints;
    std::uint64_t constraint_hash = 0;
};

struct ConstraintCompilationResult {
    CompiledConstraintSet compiled;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;

    [[nodiscard]] bool ok() const noexcept {
        return errors.empty();
    }
};

class ConstraintCompiler {
public:
    [[nodiscard]] ConstraintCompilationResult compile(
        const Rulebook& rulebook,
        const std::vector<BooleanVariable>& variables
    ) const;
};

[[nodiscard]] std::uint64_t hash_compiled_constraints(
    const CompiledConstraintSet& compiled
) noexcept;

}  // namespace trading_engine::oracle
