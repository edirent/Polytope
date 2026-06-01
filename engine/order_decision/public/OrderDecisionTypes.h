#pragma once

#include "oracle/public/CandidateBundle.h"

#include <cstdint>

namespace trading_engine::order_decision {

using Side = trading_engine::oracle::Side;

inline constexpr std::uint16_t kMaxOrderDecisionLegs = 16;

enum class OrderDecisionType : std::uint8_t {
    NoTrade,
    PaperOrderDecision,

    RejectNoDepth,
    RejectLowEdge,
    RejectInvalidBundle,
    RejectUnsupportedSide,
    RejectRiskBudget,
    RejectPartialFillRisk,
    RejectExpiredIntent,
    RejectPriceProtection,
    RejectInternalError
};

[[nodiscard]] const char* to_string(OrderDecisionType type) noexcept;

}  // namespace trading_engine::order_decision
