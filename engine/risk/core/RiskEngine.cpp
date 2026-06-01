#include "engine/risk/core/RiskEngine.h"

#include "engine/common/math/RiskMath.h"

#include <algorithm>

namespace trading_engine::risk {

namespace {

[[nodiscard]] RiskPipelineResult reject_decision(
    RiskRejectReason reason,
    const order_decision::OrderDecisionLite& order_decision,
    const RiskInputView& input,
    const char* detail
) {
    RiskPipelineResult result;
    result.decision = make_rejected_decision(reason, detail);
    result.decision.intent_id = order_decision.source_intent_id;
    result.decision.bundle_id = order_decision.bundle_id;
    result.decision.oracle_artifact_hash = order_decision.oracle_artifact_hash;
    result.decision.bundle_hash = order_decision.bundle_hash;
    result.decision.snapshot_version_hash =
        order_decision.snapshot_version_hash;
    if (input.policy != nullptr) {
        result.decision.policy_version = input.policy->policy_version;
        result.decision.policy_hash = input.policy->policy_hash != 0
            ? input.policy->policy_hash
            : compute_policy_hash(*input.policy);
    }
    result.result.intents_evaluated = 1;
    result.result.intents_rejected = 1;
    return result;
}

[[nodiscard]] signal::OpportunityIntent intent_from_decision(
    const signal::OpportunityIntent& source,
    const order_decision::OrderDecisionLite& decision,
    const RiskPolicySnapshot& policy
) {
    auto intent = source;
    intent.status = signal::IntentStatus::PaperOpportunity;
    intent.bundle_id = decision.bundle_id;
    intent.guaranteed_payout_tick = decision.guaranteed_payout_tick;
    intent.estimated_cost_tick = decision.estimated_total_cost_tick;
    intent.estimated_fee_tick = decision.estimated_fee_tick;
    intent.latency_buffer_tick = decision.latency_buffer_tick;
    intent.slippage_buffer_tick = decision.slippage_buffer_tick;
    intent.estimated_edge_tick = decision.total_edge_tick;
    intent.bundle_qty = decision.chosen_bundle_qty;
    intent.original_bundle_qty = decision.chosen_bundle_qty;
    intent.unit_edge_tick = decision.unit_edge_tick;
    intent.total_edge_tick = decision.total_edge_tick;
    intent.edge_bps = decision.edge_bps;
    intent.snapshot_version_hash = decision.snapshot_version_hash;
    intent.oracle_artifact_hash = decision.oracle_artifact_hash;
    intent.bundle_hash = decision.bundle_hash;
    intent.expires_at_ns = decision.expires_at_ns;
    intent.enough_depth = true;
    intent.leg_count = decision.leg_count;
    for (std::uint16_t i = 0; i < decision.leg_count; ++i) {
        const auto& source_leg = decision.legs[i];
        auto& target = intent.legs[i];
        target.asset_index = source_leg.asset_index;
        target.side = source_leg.side;
        target.quantity_lots = 1;
        target.requested_qty_lots = source_leg.quantity_lots;
        target.executable_qty_lots = source_leg.quantity_lots;
        target.depth_margin_bps = std::max<std::int64_t>(
            10'000,
            policy.min_depth_margin_bps
        );
        target.enough_depth = true;
        target.estimated_vwap_tick = source_leg.estimated_vwap_tick;
        target.worst_price_tick = source_leg.limit_price_tick;
        target.estimated_cost_tick = source_leg.estimated_cost_tick;
    }
    return intent;
}

}  // namespace

RiskEngine::RiskEngine() = default;

RiskEngine::RiskEngine(RiskRuntimeContext runtime_context)
    : runtime_(runtime_context) {}

RiskPipelineResult RiskEngine::evaluate(
    const signal::OpportunityIntent& intent,
    const RiskEvaluationContext& context
) {
    auto* reservations = reservation_book();
    reservations->expire_old(context.now_ns);
    ledger_snapshot_cache_ = runtime_.ledger != nullptr
        ? runtime_.ledger->snapshot()
        : reservations->snapshot();

    RiskInputView view;
    view.intent = &intent;
    view.snapshots = context.latest_snapshots.data();
    view.snapshot_count = static_cast<std::uint16_t>(
        context.latest_snapshots.size()
    );
    view.snapshot_version_hash = context.latest_snapshot_version_hash;
    view.now_ns = context.now_ns;
    view.policy = &context.policy;
    view.ledger = &ledger_snapshot_cache_;

    return pipeline_.evaluate_view(
        view,
        reservations,
        context.decision_publisher != nullptr
            ? context.decision_publisher
            : runtime_.publisher,
        context.enable_full_audit_trace || runtime_.enable_full_audit_trace
    );
}

RiskPipelineResult RiskEngine::evaluate(const RiskInputView& input) {
    return evaluate_view(input, nullptr, false);
}

RiskPipelineResult RiskEngine::evaluate_view(
    const RiskInputView& input,
    IRiskDecisionPublisher* decision_publisher,
    bool enable_full_audit_trace
) {
    auto* reservations = reservation_book();
    reservations->expire_old(input.now_ns);
    if (runtime_.ledger != nullptr) {
        ledger_snapshot_cache_ = runtime_.ledger->snapshot();
    } else if (input.ledger != nullptr) {
        ledger_snapshot_cache_ = *input.ledger;
    } else {
        ledger_snapshot_cache_ = reservations->snapshot();
    }

    auto view = input;
    view.ledger = &ledger_snapshot_cache_;
    if (view.policy == nullptr) {
        view.policy = runtime_.policy;
    }

    return pipeline_.evaluate_view(
        view,
        reservations,
        decision_publisher != nullptr ? decision_publisher : runtime_.publisher,
        enable_full_audit_trace || runtime_.enable_full_audit_trace
    );
}

RiskPipelineResult RiskEngine::evaluate_decision(
    const signal::OpportunityIntent& intent,
    const order_decision::OrderDecisionLite& decision,
    const RiskInputView& input,
    IRiskDecisionPublisher* decision_publisher,
    bool enable_full_audit_trace
) {
    const auto* policy = input.policy != nullptr ? input.policy : runtime_.policy;
    if (policy == nullptr) {
        return reject_decision(
            RiskRejectReason::InvalidIntent,
            decision,
            input,
            "missing input policy"
        );
    }
    if (decision.type !=
        order_decision::OrderDecisionType::PaperOrderDecision) {
        return reject_decision(
            RiskRejectReason::InvalidIntent,
            decision,
            input,
            "order decision is not paper decision"
        );
    }
    if (decision.chosen_bundle_qty <= 0 ||
        decision.estimated_total_cost_tick <= 0 ||
        decision.leg_count == 0 ||
        decision.leg_count > order_decision::kMaxOrderDecisionLegs) {
        return reject_decision(
            RiskRejectReason::InvalidIntent,
            decision,
            input,
            "invalid order decision size"
        );
    }
    if (decision.expires_at_ns <= input.now_ns) {
        return reject_decision(
            RiskRejectReason::ExpiredIntent,
            decision,
            input,
            "order decision expired"
        );
    }
    if (decision.source_intent_id != intent.intent_id ||
        decision.bundle_id != intent.bundle_id) {
        return reject_decision(
            RiskRejectReason::InvalidIntent,
            decision,
            input,
            "decision intent mismatch"
        );
    }
    if (decision.snapshot_version_hash != intent.snapshot_version_hash) {
        return reject_decision(
            RiskRejectReason::InvalidIntent,
            decision,
            input,
            "decision snapshot hash mismatch"
        );
    }
    if (order_decision::compute_order_decision_hash(decision) !=
        decision.decision_hash) {
        return reject_decision(
            RiskRejectReason::InvalidIntent,
            decision,
            input,
            "decision hash mismatch"
        );
    }

    const auto policy_hash = policy->policy_hash != 0
        ? policy->policy_hash
        : compute_policy_hash(*policy);
    if (decision.policy_hash != policy_hash) {
        return reject_decision(
            RiskRejectReason::InvalidIntent,
            decision,
            input,
            "policy hash mismatch"
        );
    }
    if (policy->max_total_cost_tick > 0 &&
        decision.estimated_total_cost_tick > policy->max_total_cost_tick) {
        return reject_decision(
            RiskRejectReason::CostLimit,
            decision,
            input,
            "decision cost exceeds policy"
        );
    }
    if (!common::math::passes_edge_thresholds(
            decision.unit_edge_tick,
            decision.total_edge_tick,
            decision.edge_bps,
            *policy
        )) {
        return reject_decision(
            RiskRejectReason::LowTotalEdge,
            decision,
            input,
            "decision edge below policy"
        );
    }

    const auto decision_intent =
        intent_from_decision(intent, decision, *policy);
    auto view = input;
    view.policy = policy;
    view.intent = &decision_intent;
    return evaluate_view(view, decision_publisher, enable_full_audit_trace);
}

RiskPipelineResult RiskEngine::evaluate_decision(
    const signal::OpportunityIntent& intent,
    const order_decision::OrderDecision& decision,
    const RiskInputView& input,
    IRiskDecisionPublisher* decision_publisher,
    bool enable_full_audit_trace
) {
    return evaluate_decision(
        intent,
        order_decision::to_order_decision_lite(decision),
        input,
        decision_publisher,
        enable_full_audit_trace
    );
}

RiskPipelineResult RiskEngine::evaluate_decision(
    const order_decision::OrderDecisionLite& decision,
    const RiskInputView& input,
    IRiskDecisionPublisher* decision_publisher,
    bool enable_full_audit_trace
) {
    if (input.intent == nullptr) {
        return reject_decision(
            RiskRejectReason::InvalidIntent,
            decision,
            input,
            "missing source intent"
        );
    }
    return evaluate_decision(
        *input.intent,
        decision,
        input,
        decision_publisher,
        enable_full_audit_trace
    );
}

RiskPipelineResult RiskEngine::evaluate_decision(
    const order_decision::OrderDecision& decision,
    const RiskInputView& input,
    IRiskDecisionPublisher* decision_publisher,
    bool enable_full_audit_trace
) {
    return evaluate_decision(
        order_decision::to_order_decision_lite(decision),
        input,
        decision_publisher,
        enable_full_audit_trace
    );
}

void RiskEngine::set_runtime_context(RiskRuntimeContext runtime_context) {
    runtime_ = runtime_context;
}

RiskLedgerSnapshot RiskEngine::ledger_snapshot() const {
    return reservation_book()->snapshot();
}

void RiskEngine::release_reservation(std::uint64_t reservation_id) {
    reservation_book()->release(reservation_id);
}

void RiskEngine::expire_old(std::uint64_t now_ns) {
    reservation_book()->expire_old(now_ns);
}

ReservationBook* RiskEngine::reservation_book() noexcept {
    return runtime_.reservation_book != nullptr
        ? runtime_.reservation_book
        : &reservations_;
}

const ReservationBook* RiskEngine::reservation_book() const noexcept {
    return runtime_.reservation_book != nullptr
        ? runtime_.reservation_book
        : &reservations_;
}

}  // namespace trading_engine::risk
