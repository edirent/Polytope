#include "oracle/compiler/BooleanConstraintBuilder.h"

namespace trading_engine::oracle {

namespace {

bool append_var_id(
    const std::string& variable_key,
    const BooleanConstraintBuilder::VariableIndex& variable_index,
    LinearBooleanConstraint* constraint,
    std::vector<std::string>* errors
) {
    const auto it = variable_index.find(variable_key);
    if (it == variable_index.end()) {
        errors->push_back("unknown variable: " + variable_key);
        return false;
    }

    constraint->var_ids.push_back(it->second);
    return true;
}

}  // namespace

BooleanConstraintBuildResult BooleanConstraintBuilder::exactly_one(
    const std::vector<std::string>& variable_keys,
    const VariableIndex& variable_index
) const {
    return sum_constraint(variable_keys, variable_index, ConstraintOp::Equal, 1);
}

BooleanConstraintBuildResult BooleanConstraintBuilder::at_most_one(
    const std::vector<std::string>& variable_keys,
    const VariableIndex& variable_index
) const {
    return sum_constraint(
        variable_keys,
        variable_index,
        ConstraintOp::LessEqual,
        1
    );
}

BooleanConstraintBuildResult BooleanConstraintBuilder::at_least_one(
    const std::vector<std::string>& variable_keys,
    const VariableIndex& variable_index
) const {
    return sum_constraint(
        variable_keys,
        variable_index,
        ConstraintOp::GreaterEqual,
        1
    );
}

BooleanConstraintBuildResult BooleanConstraintBuilder::implies(
    const std::vector<std::string>& variable_keys,
    const VariableIndex& variable_index
) const {
    BooleanConstraintBuildResult result;
    if (variable_keys.size() != 2) {
        result.errors.push_back("Implies requires exactly two variables");
        return result;
    }

    if (!append_var_id(
            variable_keys[0],
            variable_index,
            &result.constraint,
            &result.errors
        )) {
        return result;
    }
    if (!append_var_id(
            variable_keys[1],
            variable_index,
            &result.constraint,
            &result.errors
        )) {
        return result;
    }

    result.constraint.coeffs = {1, -1};
    result.constraint.op = ConstraintOp::LessEqual;
    result.constraint.rhs = 0;
    return result;
}

BooleanConstraintBuildResult BooleanConstraintBuilder::sum_constraint(
    const std::vector<std::string>& variable_keys,
    const VariableIndex& variable_index,
    ConstraintOp op,
    std::int32_t rhs
) const {
    BooleanConstraintBuildResult result;
    if (variable_keys.empty()) {
        result.errors.push_back("constraint has empty variable set");
        return result;
    }

    for (const auto& variable_key : variable_keys) {
        append_var_id(
            variable_key,
            variable_index,
            &result.constraint,
            &result.errors
        );
    }

    result.constraint.coeffs.assign(result.constraint.var_ids.size(), 1);
    result.constraint.op = op;
    result.constraint.rhs = rhs;
    return result;
}

}  // namespace trading_engine::oracle
