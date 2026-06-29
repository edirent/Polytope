#pragma once

#include <cstddef>
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
    RejectUnsupportedSide,
    RejectSpreadTooTight,
    RejectSpreadTooWide,
    RejectFairValueDeviation,
    RejectRewardConfigMissing,
    RejectRewardSpreadTooWide,
    RejectRewardSizeTooSmall,
    RejectStaleSpot,
    RejectStaleVol,
    RejectLowExternalConfidence,
    RejectCanonicalExposureLimit,
    RejectPortfolioTouchLimit
};

inline constexpr std::size_t kQuoteRiskDecisionTypeCount = 24;

[[nodiscard]] inline const char* quote_risk_decision_type_name(
    QuoteRiskDecisionType decision
) noexcept {
    switch (decision) {
        case QuoteRiskDecisionType::Approve:
            return "approve";
        case QuoteRiskDecisionType::RejectInvalidQuote:
            return "reject_invalid_quote";
        case QuoteRiskDecisionType::RejectExpiredQuote:
            return "reject_expired_quote";
        case QuoteRiskDecisionType::RejectStaleBook:
            return "reject_stale_book";
        case QuoteRiskDecisionType::RejectCrossedBook:
            return "reject_crossed_book";
        case QuoteRiskDecisionType::RejectBookNotUsable:
            return "reject_book_not_usable";
        case QuoteRiskDecisionType::RejectInventoryLimit:
            return "reject_inventory_limit";
        case QuoteRiskDecisionType::RejectExposureLimit:
            return "reject_exposure_limit";
        case QuoteRiskDecisionType::RejectDuplicateQuote:
            return "reject_duplicate_quote";
        case QuoteRiskDecisionType::RejectQuoteTooFrequent:
            return "reject_quote_too_frequent";
        case QuoteRiskDecisionType::RejectKillSwitch:
            return "reject_kill_switch";
        case QuoteRiskDecisionType::RejectLowEdgeToFair:
            return "reject_low_edge_to_fair";
        case QuoteRiskDecisionType::RejectUnsupportedSide:
            return "reject_unsupported_side";
        case QuoteRiskDecisionType::RejectSpreadTooTight:
            return "reject_spread_too_tight";
        case QuoteRiskDecisionType::RejectSpreadTooWide:
            return "reject_spread_too_wide";
        case QuoteRiskDecisionType::RejectFairValueDeviation:
            return "reject_fair_value_deviation";
        case QuoteRiskDecisionType::RejectRewardConfigMissing:
            return "reject_reward_config_missing";
        case QuoteRiskDecisionType::RejectRewardSpreadTooWide:
            return "reject_reward_spread_too_wide";
        case QuoteRiskDecisionType::RejectRewardSizeTooSmall:
            return "reject_reward_size_too_small";
        case QuoteRiskDecisionType::RejectStaleSpot:
            return "reject_stale_spot";
        case QuoteRiskDecisionType::RejectStaleVol:
            return "reject_stale_vol";
        case QuoteRiskDecisionType::RejectLowExternalConfidence:
            return "reject_low_external_confidence";
        case QuoteRiskDecisionType::RejectCanonicalExposureLimit:
            return "reject_canonical_exposure_limit";
        case QuoteRiskDecisionType::RejectPortfolioTouchLimit:
            return "reject_portfolio_touch_limit";
    }
    return "unknown";
}

inline constexpr std::uint8_t kQuoteRiskExposureBreachReasonCode = 3;

[[nodiscard]] inline std::uint8_t quote_risk_rejection_reason_code(
    QuoteRiskDecisionType decision
) noexcept {
    switch (decision) {
        case QuoteRiskDecisionType::RejectInventoryLimit:
        case QuoteRiskDecisionType::RejectExposureLimit:
        case QuoteRiskDecisionType::RejectCanonicalExposureLimit:
        case QuoteRiskDecisionType::RejectPortfolioTouchLimit:
            return kQuoteRiskExposureBreachReasonCode;
        default:
            return 0;
    }
}

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
