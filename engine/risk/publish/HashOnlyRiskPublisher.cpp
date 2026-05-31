#include "engine/risk/publish/HashOnlyRiskPublisher.h"

#include "engine/risk/core/RiskContext.h"

#include <limits>

namespace trading_engine::risk {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void mix_u64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        *hash ^= static_cast<std::uint8_t>((value >> shift) & 0xffU);
        *hash *= kFnvPrime;
    }
}

void mix_i64(std::uint64_t* hash, std::int64_t value) noexcept {
    mix_u64(hash, static_cast<std::uint64_t>(value));
}

[[nodiscard]] std::int64_t saturating_mul_i64(
    std::int64_t lhs,
    std::int64_t rhs
) noexcept {
    const auto value =
        static_cast<__int128>(lhs) * static_cast<__int128>(rhs);
    if (value > std::numeric_limits<std::int64_t>::max()) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (value < std::numeric_limits<std::int64_t>::min()) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(value);
}

[[nodiscard]] std::int64_t risk_total_edge_tick(
    const RiskPipelineResult& result
) noexcept {
    if (!result.decision.approved() || !result.cost.ok) {
        return 0;
    }

    auto edge = saturating_mul_i64(
        result.approved_intent.intent.guaranteed_payout_tick,
        result.cost.risk_bundle_qty
    );
    edge -= result.cost.risk_total_cost_tick;
    edge -= result.cost.fee_tick;
    edge -= result.cost.slippage_buffer_tick;
    edge -= result.cost.latency_buffer_tick;
    return edge;
}

}  // namespace

HashOnlyRiskPublisher::HashOnlyRiskPublisher(std::size_t reserve_capacity) {
    if (reserve_capacity > 0) {
        records_.reserve(reserve_capacity);
    }
}

void HashOnlyRiskPublisher::publish_decision(
    const RiskDecision& decision,
    const RiskAuditTrace& trace
) {
    records_.push_back({
        .decision_id = decision.decision_id,
        .intent_id = decision.intent_id != 0 ? decision.intent_id :
                                               trace.intent_id,
        .bundle_id = decision.bundle_id != 0 ? decision.bundle_id :
                                               trace.bundle_id,
        .decision = decision.status,
        .reject_flags = static_cast<std::uint64_t>(decision.reject_reason),
        .risk_total_edge_tick = 0,
        .policy_hash = decision.policy_hash,
        .reservation_id = 0
    });
}

void HashOnlyRiskPublisher::publish_result(const RiskPipelineResult& result) {
    const auto& decision = result.decision;
    records_.push_back({
        .decision_id = decision.decision_id,
        .intent_id = decision.intent_id != 0 ? decision.intent_id :
                                               result.audit_trace.intent_id,
        .bundle_id = decision.bundle_id != 0 ? decision.bundle_id :
                                               result.audit_trace.bundle_id,
        .decision = decision.status,
        .reject_flags = static_cast<std::uint64_t>(decision.reject_reason),
        .risk_total_edge_tick = risk_total_edge_tick(result),
        .policy_hash = decision.policy_hash,
        .reservation_id = result.reservation.reservation_id
    });
}

void HashOnlyRiskPublisher::reserve(std::size_t capacity) {
    records_.reserve(capacity);
}

void HashOnlyRiskPublisher::clear() {
    records_.clear();
}

const std::vector<HashOnlyRiskRecord>& HashOnlyRiskPublisher::records()
    const noexcept {
    return records_;
}

std::uint64_t HashOnlyRiskPublisher::output_hash() const noexcept {
    if (records_.empty()) {
        return 0;
    }

    auto hash = kFnvOffset;
    for (const auto& record : records_) {
        mix_u64(&hash, record.decision_id);
        mix_u64(&hash, record.intent_id);
        mix_u64(&hash, record.bundle_id);
        mix_u64(&hash, static_cast<std::uint64_t>(record.decision));
        mix_u64(&hash, record.reject_flags);
        mix_i64(&hash, record.risk_total_edge_tick);
        mix_u64(&hash, record.policy_hash);
        mix_u64(&hash, record.reservation_id);
    }
    return hash;
}

}  // namespace trading_engine::risk
