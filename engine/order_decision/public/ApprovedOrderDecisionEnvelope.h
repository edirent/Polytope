#pragma once

#include "engine/order_decision/public/OrderDecision.h"
#include "engine/risk/public/ApprovedIntent.h"

#include <cstdint>
#include <utility>

namespace trading_engine::order_decision {

struct ApprovedOrderDecisionEnvelope {
    trading_engine::risk::ApprovedIntent approved;
    OrderDecisionLite decision;

    std::uint64_t source_intent_id = 0;
    std::uint64_t bundle_id = 0;

    std::uint64_t decision_hash = 0;
    std::uint64_t approval_hash = 0;

    std::uint64_t created_ts_ns = 0;
};

namespace detail {

inline constexpr std::uint64_t kApprovedEnvelopeFnvOffset =
    14695981039346656037ULL;
inline constexpr std::uint64_t kApprovedEnvelopeFnvPrime = 1099511628211ULL;

inline void mix_u64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        *hash ^= static_cast<std::uint8_t>((value >> shift) & 0xffU);
        *hash *= kApprovedEnvelopeFnvPrime;
    }
}

}  // namespace detail

[[nodiscard]] inline std::uint64_t compute_approved_intent_hash(
    const trading_engine::risk::ApprovedIntent& approved
) noexcept {
    auto hash = detail::kApprovedEnvelopeFnvOffset;
    detail::mix_u64(&hash, approved.intent.intent_id);
    detail::mix_u64(&hash, approved.intent.bundle_id);
    detail::mix_u64(&hash, approved.intent.idempotency_hash);
    detail::mix_u64(&hash, approved.decision.decision_id);
    detail::mix_u64(&hash, approved.decision.intent_id);
    detail::mix_u64(&hash, approved.decision.bundle_id);
    detail::mix_u64(&hash, approved.decision.idempotency_hash);
    detail::mix_u64(
        &hash,
        static_cast<std::uint64_t>(approved.decision.status)
    );
    detail::mix_u64(
        &hash,
        static_cast<std::uint64_t>(approved.decision.reject_reason)
    );
    detail::mix_u64(&hash, approved.reservation_id_hash);
    detail::mix_u64(&hash, approved.approved_at_ns);
    detail::mix_u64(&hash, approved.expires_at_ns);
    return hash == 0 ? 1 : hash;
}

[[nodiscard]] inline ApprovedOrderDecisionEnvelope
make_approved_order_decision_envelope(
    trading_engine::risk::ApprovedIntent approved,
    OrderDecisionLite decision,
    std::uint64_t created_ts_ns
) noexcept {
    ApprovedOrderDecisionEnvelope envelope;
    envelope.source_intent_id = decision.source_intent_id;
    envelope.bundle_id = decision.bundle_id;
    envelope.decision_hash = decision.decision_hash;
    envelope.approval_hash = compute_approved_intent_hash(approved);
    envelope.created_ts_ns = created_ts_ns;
    envelope.approved = std::move(approved);
    envelope.decision = std::move(decision);
    return envelope;
}

[[nodiscard]] inline ApprovedOrderDecisionEnvelope
make_approved_order_decision_envelope(
    trading_engine::risk::ApprovedIntent approved,
    const OrderDecision& decision,
    std::uint64_t created_ts_ns
) noexcept {
    return make_approved_order_decision_envelope(
        std::move(approved),
        to_order_decision_lite(decision),
        created_ts_ns
    );
}

}  // namespace trading_engine::order_decision
