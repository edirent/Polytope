#pragma once

#include <cstdint>
#include <string>

namespace trading_engine::risk {

enum class QuoteRiskDecisionType : std::uint8_t {
    Approve,
    RejectInvalidQuote,
    RejectExpiredQuote,
    RejectStaleBook,
    RejectCrossedBook,
    RejectBookNotUsable,
    RejectInventoryLimit,
    RejectExposureLimit,
    RejectDuplicateQuote,
    RejectQuoteTooFrequent,
    RejectKillSwitch,
    RejectLowEdgeToFair,
    RejectUnsupportedSide
};

struct QuoteRiskDecision {
    std::uint64_t decision_id = 0;
    std::uint64_t quote_intent_id = 0;
    std::uint64_t quote_group_id = 0;

    QuoteRiskDecisionType decision =
        QuoteRiskDecisionType::RejectInvalidQuote;

    std::uint64_t policy_hash = 0;
    std::uint64_t snapshot_version_hash = 0;
    std::uint64_t idempotency_hash = 0;
    std::uint64_t decision_ts_ns = 0;

    std::int64_t bid_notional_tick = 0;
    std::int64_t ask_notional_tick = 0;
    std::int64_t total_notional_tick = 0;

    std::string reason;
};

[[nodiscard]] std::uint64_t compute_quote_risk_decision_hash(
    const QuoteRiskDecision& decision
) noexcept;

}  // namespace trading_engine::risk
