#include "oracle/enumerate/FeasibilityChecker.h"

namespace trading_engine::oracle {

namespace {

bool evaluate(
    std::int32_t lhs,
    ConstraintOp op,
    std::int32_t rhs
) noexcept {
    switch (op) {
        case ConstraintOp::Equal:
            return lhs == rhs;
        case ConstraintOp::LessEqual:
            return lhs <= rhs;
        case ConstraintOp::GreaterEqual:
            return lhs >= rhs;
    }

    return false;
}

}  // namespace

FeasibilityCheckResult FeasibilityChecker::check(
    const CompiledConstraintSet& compiled,
    const StateBitset& state
) const {
    FeasibilityCheckResult result;

    for (std::size_t i = 0; i < compiled.constraints.size(); ++i) {
        const auto& constraint = compiled.constraints[i];
        if (constraint.var_ids.size() != constraint.coeffs.size()) {
            result.code = OracleErrorCode::InvalidInput;
            result.errors.push_back(
                "constraint " + std::to_string(i) +
                " has mismatched var_ids and coeffs"
            );
            return result;
        }

        std::int32_t lhs = 0;
        for (std::size_t term = 0; term < constraint.var_ids.size(); ++term) {
            const std::int32_t x = state.test(constraint.var_ids[term]) ? 1 : 0;
            lhs += constraint.coeffs[term] * x;
        }

        if (!evaluate(lhs, constraint.op, constraint.rhs)) {
            result.feasible = false;
            return result;
        }
    }

    result.feasible = true;
    return result;
}

}  // namespace trading_engine::oracle
