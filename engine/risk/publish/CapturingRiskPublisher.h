#pragma once

#include "engine/risk/publish/RiskDecisionPublisher.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace trading_engine::risk {

struct PublishedRiskDecision {
    RiskDecision decision;
    RiskAuditTrace trace;
};

struct PublishedRiskDecisionFast {
    RiskDecisionStatus status = RiskDecisionStatus::Rejected;
    RiskRejectReason reject_reason = RiskRejectReason::NotEvaluated;

    std::uint64_t decision_id = 0;
    std::uint64_t intent_id = 0;
    std::uint64_t bundle_id = 0;
    std::uint64_t policy_version = 0;
    std::uint64_t policy_hash = 0;

    std::uint8_t audit_step_count = 0;
    std::array<RiskAuditStepLite, kMaxRiskAuditSteps> audit_steps{};
};

class CapturingRiskPublisher final : public IRiskDecisionPublisher {
public:
    void publish_decision(
        const RiskDecision& decision,
        const RiskAuditTrace& trace
    ) override;

    [[nodiscard]] const std::vector<PublishedRiskDecision>& decisions()
        const noexcept;

private:
    std::vector<PublishedRiskDecision> decisions_;
};

class CapturingRiskPublisherFast final : public IRiskDecisionPublisher {
public:
    void publish_decision(
        const RiskDecision& decision,
        const RiskAuditTrace& trace
    ) override;

    void reserve(std::size_t capacity);

    [[nodiscard]] const std::vector<PublishedRiskDecisionFast>& decisions()
        const noexcept;

private:
    std::vector<PublishedRiskDecisionFast> decisions_;
};

}  // namespace trading_engine::risk
