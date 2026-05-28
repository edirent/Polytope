#include "oracle/enumerate/StateEnumerator.h"

#include "oracle/enumerate/FeasibilityChecker.h"
#include "oracle/enumerate/StateBitset.h"

#include <limits>

namespace trading_engine::oracle {

StateEnumerationResult StateEnumerator::enumerate(
    const CompiledConstraintSet& compiled
) const {
    StateEnumerationResult result;
    const auto variable_count =
        static_cast<std::uint32_t>(compiled.variables.size());

    if (variable_count > kMaxBruteforceVariables) {
        result.code = OracleErrorCode::TooManyVariables;
        result.errors.push_back("state enumeration supports <= 32 variables");
        return result;
    }

    const std::uint64_t state_count =
        variable_count == 64
            ? std::numeric_limits<std::uint64_t>::max()
            : (1ULL << variable_count);

    FeasibilityChecker checker;
    for (std::uint64_t mask = 0; mask < state_count; ++mask) {
        StateBitset bitset = state_bitset_from_mask(mask);
        const auto check = checker.check(compiled, bitset);
        if (!check.ok()) {
            result.code = check.code;
            result.errors.insert(
                result.errors.end(),
                check.errors.begin(),
                check.errors.end()
            );
            return result;
        }

        if (check.feasible) {
            result.feasible_states.push_back(
                FeasibleState{mask, std::move(bitset.words)}
            );
        }
    }

    return result;
}

}  // namespace trading_engine::oracle
