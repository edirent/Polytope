#pragma once

#include "oracle/compiler/BooleanVariable.h"
#include "oracle/enumerate/FeasibleState.h"
#include "oracle/payoff/PayoffMatrix.h"

#include <string>
#include <vector>

namespace trading_engine::oracle {

struct PayoffMatrixBuildResult {
    PayoffMatrix matrix;
    std::vector<std::string> errors;

    [[nodiscard]] bool ok() const noexcept {
        return errors.empty();
    }
};

class PayoffMatrixBuilder {
public:
    [[nodiscard]] PayoffMatrixBuildResult build(
        const std::vector<BooleanVariable>& variables,
        const std::vector<FeasibleState>& feasible_states
    ) const;

    [[nodiscard]] bool write(
        const PayoffMatrix& matrix,
        const std::string& directory,
        std::vector<std::string>* errors = nullptr
    ) const;
};

[[nodiscard]] std::uint64_t hash_payoff_matrix(
    const PayoffMatrix& matrix
) noexcept;

}  // namespace trading_engine::oracle
