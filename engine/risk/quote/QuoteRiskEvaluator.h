#pragma once

#include "engine/risk/quote/QuoteRiskTypes.h"

namespace trading_engine::risk {

class QuoteRiskEvaluator {
public:
    [[nodiscard]] QuoteRiskResult evaluate(const QuoteRiskInput& input) const;
};

}  // namespace trading_engine::risk
