#pragma once

#include "engine/strategy/market_making/public/MarketMakingTypes.h"

namespace trading_engine::strategy::market_making {

[[nodiscard]] inline bool quote_status_terminal(QuoteStatus status) noexcept {
    return status == QuoteStatus::Filled || status == QuoteStatus::Cancelled ||
           status == QuoteStatus::Expired || status == QuoteStatus::Replaced ||
           status == QuoteStatus::Rejected;
}

[[nodiscard]] inline bool can_transition_quote_status(
    QuoteStatus from,
    QuoteStatus to
) noexcept {
    if (from == to) {
        return true;
    }
    if (quote_status_terminal(from)) {
        return false;
    }
    if (to == QuoteStatus::Rejected || to == QuoteStatus::Expired ||
        to == QuoteStatus::Cancelled || to == QuoteStatus::Replaced) {
        return true;
    }
    switch (from) {
        case QuoteStatus::Created:
            return to == QuoteStatus::RiskApproved ||
                   to == QuoteStatus::PostedPaper ||
                   to == QuoteStatus::ActivePaper;
        case QuoteStatus::RiskApproved:
            return to == QuoteStatus::PostedPaper ||
                   to == QuoteStatus::ActivePaper;
        case QuoteStatus::PostedPaper:
            return to == QuoteStatus::ActivePaper;
        case QuoteStatus::ActivePaper:
            return to == QuoteStatus::PartiallyFilled ||
                   to == QuoteStatus::Filled ||
                   to == QuoteStatus::CancelRequested;
        case QuoteStatus::PartiallyFilled:
            return to == QuoteStatus::Filled ||
                   to == QuoteStatus::CancelRequested;
        case QuoteStatus::CancelRequested:
            return to == QuoteStatus::Cancelled;
        default:
            return false;
    }
}

}  // namespace trading_engine::strategy::market_making
