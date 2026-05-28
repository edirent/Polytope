#pragma once

#include "oracle/compiler/ConstraintCompiler.h"
#include "oracle/enumerate/FeasibleState.h"
#include "oracle/public/OracleError.h"

#include <string>
#include <vector>

namespace trading_engine::oracle {

struct StateEnumerationResult {
    OracleErrorCode code = OracleErrorCode::None;
    std::vector<FeasibleState> feasible_states;
    std::vector<std::string> errors;

    [[nodiscard]] bool ok() const noexcept {
        return code == OracleErrorCode::None && errors.empty();
    }
};

class StateEnumerator {
public:
    static constexpr std::uint32_t kMaxBruteforceVariables = 32;

    [[nodiscard]] StateEnumerationResult enumerate(
        const CompiledConstraintSet& compiled
    ) const;
};

}  // namespace trading_engine::oracle
