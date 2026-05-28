#pragma once

#include "oracle/compiler/ConstraintCompiler.h"
#include "oracle/enumerate/StateBitset.h"
#include "oracle/public/OracleError.h"

#include <string>
#include <vector>

namespace trading_engine::oracle {

struct FeasibilityCheckResult {
    OracleErrorCode code = OracleErrorCode::None;
    bool feasible = false;
    std::vector<std::string> errors;

    [[nodiscard]] bool ok() const noexcept {
        return code == OracleErrorCode::None && errors.empty();
    }
};

class FeasibilityChecker {
public:
    [[nodiscard]] FeasibilityCheckResult check(
        const CompiledConstraintSet& compiled,
        const StateBitset& state
    ) const;
};

}  // namespace trading_engine::oracle
