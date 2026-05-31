#pragma once

#include "engine/risk/publish/RiskDecisionPublisher.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace trading_engine::risk {

struct HashOnlyRiskRecord {
    std::uint64_t decision_id = 0;
    std::uint64_t intent_id = 0;
    std::uint64_t bundle_id = 0;

    RiskDecisionStatus decision = RiskDecisionStatus::Rejected;
    std::uint64_t reject_flags = 0;

    std::int64_t risk_total_edge_tick = 0;
    std::uint64_t policy_hash = 0;
    std::uint64_t reservation_id = 0;
};

class HashOnlyRiskPublisher final : public IRiskDecisionPublisher {
public:
    explicit HashOnlyRiskPublisher(std::size_t reserve_capacity = 0);

    void publish_decision(
        const RiskDecision& decision,
        const RiskAuditTrace& trace
    ) override;

    void publish_result(const RiskPipelineResult& result) override;

    void reserve(std::size_t capacity);
    void clear();

    [[nodiscard]] const std::vector<HashOnlyRiskRecord>& records()
        const noexcept;

    [[nodiscard]] std::uint64_t output_hash() const noexcept;

private:
    std::vector<HashOnlyRiskRecord> records_;
};

}  // namespace trading_engine::risk
