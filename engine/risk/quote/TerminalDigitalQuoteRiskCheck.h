#pragma once

#include "engine/risk/quote/QuoteRiskTypes.h"

namespace trading_engine::risk {

struct TerminalDigitalQuoteRiskCheckResult {
    bool rejected = false;
    QuoteRiskDecisionType decision = QuoteRiskDecisionType::RejectInvalidQuote;
    const char* reason = "";
};

class TerminalDigitalQuoteRiskCheck {
public:
    [[nodiscard]] TerminalDigitalQuoteRiskCheckResult evaluate(
        const QuoteRiskInput& input
    ) const noexcept;
};

}  // namespace trading_engine::risk
