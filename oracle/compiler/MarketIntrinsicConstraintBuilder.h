#pragma once

#include "oracle/compiler/BooleanVariable.h"
#include "oracle/compiler/Constraint.h"
#include "oracle/ingestion/MarketUniverseBuilder.h"

#include <vector>

namespace trading_engine::oracle {

class MarketIntrinsicConstraintBuilder {
public:
    [[nodiscard]] std::vector<LinearBooleanConstraint> build(
        const MarketUniverse& universe,
        const BooleanVariableRegistry& variables
    ) const;
};

}  // namespace trading_engine::oracle
