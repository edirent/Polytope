#pragma once

#include "engine/risk/public/RiskDecision.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::risk {

enum class RiskAuditStepCode : std::uint8_t {
    KillSwitchGuard,
    IntentValidator,
    EvidenceVerifier,
    ExpiryGuard,
    DuplicateGuard,
    RateLimitGuard,
    MarketStateGuard,
    SnapshotFreshnessGuard,
    CostRevalidator,
    EdgeGuard,
    MaxLossGuard,
    ExposureGuard,
    InventoryGuard,
    PartialFillGuard,
    ReservationBook
};

struct RiskAuditStepLite {
    RiskAuditStepCode step = RiskAuditStepCode::IntentValidator;
    bool pass = false;
    RiskDecisionType rejection = RiskDecisionType::RejectInternalError;
    std::uint64_t detail_code = 0;
};

inline constexpr std::uint8_t kMaxRiskAuditSteps = 16;

struct RiskAuditTraceLite {
    std::uint64_t decision_id = 0;
    std::uint8_t step_count = 0;
    std::array<RiskAuditStepLite, kMaxRiskAuditSteps> steps{};
};

struct RiskAuditStep {
    std::string guard_name;
    bool pass = false;
    RiskDecisionType rejection = RiskDecisionType::RejectInternalError;
    std::string reason;
};

struct RiskAuditTrace {
    std::uint64_t trace_id = 0;
    std::uint64_t decision_id = 0;
    std::uint64_t intent_id = 0;
    std::uint64_t bundle_id = 0;

    std::uint64_t policy_version = 0;
    std::uint64_t policy_hash = 0;

    RiskDecision decision;
    RiskAuditTraceLite lite;
    std::vector<RiskAuditStep> steps;
    std::vector<std::string> evidence;
};

[[nodiscard]] inline const char* risk_audit_step_name(
    RiskAuditStepCode code
) noexcept {
    switch (code) {
        case RiskAuditStepCode::KillSwitchGuard:
            return "KillSwitchGuard";
        case RiskAuditStepCode::IntentValidator:
            return "IntentValidator";
        case RiskAuditStepCode::EvidenceVerifier:
            return "IntentEvidenceVerifier";
        case RiskAuditStepCode::ExpiryGuard:
            return "IntentExpiryGuard";
        case RiskAuditStepCode::DuplicateGuard:
            return "DuplicateIntentGuard";
        case RiskAuditStepCode::RateLimitGuard:
            return "RateLimitGuard";
        case RiskAuditStepCode::MarketStateGuard:
            return "MarketStateGuard";
        case RiskAuditStepCode::SnapshotFreshnessGuard:
            return "SnapshotFreshnessGuard";
        case RiskAuditStepCode::CostRevalidator:
            return "CostRevalidator";
        case RiskAuditStepCode::EdgeGuard:
            return "EdgeGuard";
        case RiskAuditStepCode::MaxLossGuard:
            return "MaxLossGuard";
        case RiskAuditStepCode::ExposureGuard:
            return "ExposureGuard";
        case RiskAuditStepCode::InventoryGuard:
            return "InventoryGuard";
        case RiskAuditStepCode::PartialFillGuard:
            return "PartialFillGuard";
        case RiskAuditStepCode::ReservationBook:
            return "ReservationBook.try_reserve";
    }

    return "UnknownRiskAuditStep";
}

}  // namespace trading_engine::risk
