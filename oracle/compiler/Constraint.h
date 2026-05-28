#pragma once

#include <cstdint>
#include <vector>

namespace trading_engine::oracle {

enum class ConstraintOp : std::uint8_t {
    Equal,
    LessEqual,
    GreaterEqual
};

struct LinearBooleanConstraint {
    std::vector<std::uint32_t> var_ids;
    std::vector<std::int32_t> coeffs;

    ConstraintOp op = ConstraintOp::Equal;
    std::int32_t rhs = 0;
};

}  // namespace trading_engine::oracle
