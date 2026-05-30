#include "engine/risk/core/RiskPipeline.h"

#include "engine/risk/public/RiskDecision.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>

namespace trading_engine::risk {

namespace {

namespace detail {

inline constexpr std::uint64_t kOffset = 14695981039346656037ULL;
inline constexpr std::uint64_t kPrime = 1099511628211ULL;

void mix_u64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        *hash ^= (value >> shift) & 0xffU;
        *hash *= kPrime;
    }
}

void mix_i64(std::uint64_t* hash, std::int64_t value) noexcept {
    mix_u64(hash, static_cast<std::uint64_t>(value));
}

}  // namespace detail

[[nodiscard]] std::uint64_t policy_hash_for(
    const RiskPolicySnapshot& policy
) noexcept {
    return policy.policy_hash != 0 ? policy.policy_hash :
                                     compute_policy_hash(policy);
}

[[nodiscard]] RiskRejectReason reject_reason_for(
    RiskDecisionType type
) noexcept {
    switch (type) {
        case RiskDecisionType::Pass:
            return RiskRejectReason::None;
        case RiskDecisionType::RejectKillSwitch:
            return RiskRejectReason::KillSwitch;
        case RiskDecisionType::RejectExpiredIntent:
            return RiskRejectReason::ExpiredIntent;
        case RiskDecisionType::RejectDuplicateIntent:
            return RiskRejectReason::DuplicateIntent;
        case RiskDecisionType::RejectRateLimited:
            return RiskRejectReason::ApprovalRateLimit;
        case RiskDecisionType::RejectBadMarketState:
            return RiskRejectReason::BadMarketState;
        case RiskDecisionType::RejectStaleSnapshot:
            return RiskRejectReason::StaleBook;
        case RiskDecisionType::RejectInsufficientDepth:
        case RiskDecisionType::RejectReducedBundleQty:
            return RiskRejectReason::CostLimit;
        case RiskDecisionType::RejectCostDrift:
            return RiskRejectReason::CostDrift;
        case RiskDecisionType::RejectLowTotalEdge:
            return RiskRejectReason::LowTotalEdge;
        case RiskDecisionType::RejectLowUnitEdge:
            return RiskRejectReason::LowUnitEdge;
        case RiskDecisionType::RejectLowEdgeBps:
            return RiskRejectReason::LowEdgeBps;
        case RiskDecisionType::RejectCostLimit:
            return RiskRejectReason::CostLimit;
        case RiskDecisionType::RejectTotalExposureLimit:
            return RiskRejectReason::TotalExposureLimit;
        case RiskDecisionType::RejectSingleMarketExposureLimit:
            return RiskRejectReason::SingleMarketExposureLimit;
        case RiskDecisionType::RejectInventoryLimit:
            return RiskRejectReason::InventoryLimit;
        case RiskDecisionType::RejectPartialFillRisk:
            return RiskRejectReason::PartialFillRisk;
        case RiskDecisionType::RejectInternalError:
            return RiskRejectReason::InternalError;
    }

    return RiskRejectReason::InternalError;
}

void record_reject_reason(RiskRejectReason reason, RiskResult* result) {
    if (result == nullptr) {
        return;
    }
    ++result->intents_rejected;
    ++result->metrics.reject_count;

    switch (reason) {
        case RiskRejectReason::KillSwitch:
            ++result->rejected_kill_switch;
            ++result->metrics.reject_kill_switch;
            break;
        case RiskRejectReason::LowTotalEdge:
        case RiskRejectReason::LowUnitEdge:
        case RiskRejectReason::LowEdgeBps:
            ++result->rejected_low_edge;
            ++result->metrics.reject_low_edge;
            break;
        case RiskRejectReason::CostLimit:
            ++result->rejected_cost_limit;
            ++result->metrics.reject_insufficient_depth;
            break;
        case RiskRejectReason::SingleMarketExposureLimit:
        case RiskRejectReason::TotalExposureLimit:
            ++result->rejected_exposure_limit;
            ++result->metrics.reject_exposure;
            break;
        case RiskRejectReason::InventoryLimit:
            ++result->rejected_inventory_limit;
            ++result->metrics.reject_inventory;
            break;
        case RiskRejectReason::BadMarketState:
            ++result->rejected_bad_market_state;
            ++result->metrics.reject_stale_book;
            break;
        case RiskRejectReason::StaleBook:
        case RiskRejectReason::ExpiredIntent:
            ++result->rejected_stale_or_expired;
            if (reason == RiskRejectReason::ExpiredIntent) {
                ++result->metrics.reject_expired;
            } else {
                ++result->metrics.reject_stale_book;
            }
            break;
        case RiskRejectReason::SnapshotSkew:
            ++result->rejected_snapshot_freshness;
            ++result->metrics.reject_stale_book;
            break;
        case RiskRejectReason::CostDrift:
        case RiskRejectReason::SlippageLimit:
            ++result->rejected_drift_or_slippage;
            ++result->metrics.reject_cost_drift;
            break;
        case RiskRejectReason::PendingIntentLimit:
        case RiskRejectReason::ApprovalRateLimit:
        case RiskRejectReason::DuplicateIntent:
        case RiskRejectReason::DuplicateReservation:
            ++result->rejected_pending_or_rate_limit;
            if (reason == RiskRejectReason::DuplicateIntent ||
                reason == RiskRejectReason::DuplicateReservation) {
                ++result->metrics.reject_duplicate;
            } else {
                ++result->metrics.reject_rate_limited;
            }
            break;
        case RiskRejectReason::PartialFillRisk:
            ++result->rejected_partial_fill_risk;
            ++result->metrics.reject_partial_fill;
            break;
        case RiskRejectReason::InvalidIntent:
        case RiskRejectReason::MissingEvidence:
        case RiskRejectReason::InternalError:
            ++result->rejected_bad_market_state;
            ++result->metrics.reject_invalid_intent;
            break;
        case RiskRejectReason::None:
        case RiskRejectReason::NotEvaluated:
        case RiskRejectReason::RiskDisabled:
        case RiskRejectReason::MissingReservation:
            break;
    }
}

[[nodiscard]] std::uint64_t output_hash_for(
    const signal::OpportunityIntent& intent,
    const RiskDecision& decision,
    const CostRevalidationResult& cost,
    std::uint64_t reservation_id
) noexcept {
    auto hash = detail::kOffset;
    detail::mix_u64(&hash, intent.intent_id);
    detail::mix_u64(&hash, intent.bundle_id);
    detail::mix_u64(&hash, static_cast<std::uint64_t>(decision.status));
    detail::mix_u64(&hash, static_cast<std::uint64_t>(decision.reject_reason));
    detail::mix_u64(&hash, decision.policy_version);
    detail::mix_u64(&hash, decision.policy_hash);
    detail::mix_i64(&hash, cost.risk_total_cost_tick);
    detail::mix_i64(&hash, cost.risk_bundle_qty);
    detail::mix_u64(&hash, reservation_id);
    return hash;
}

[[nodiscard]] RiskPipelineResult reject_result(
    const signal::OpportunityIntent& intent,
    RiskRejectReason reason,
    std::string detail,
    const CostRevalidationResult& cost,
    RiskPipelineResult result
) {
    result.decision = make_rejected_decision(reason, std::move(detail));
    result.audit_trace.decision = result.decision;
    result.audit_trace.decision_id = result.decision.decision_id;
    result.cost = cost;
    record_reject_reason(reason, &result.result);
    result.output_hash = output_hash_for(intent, result.decision, result.cost, 0);
    result.result.output_hash = result.output_hash;
    return result;
}

void init_trace(
    RiskPipelineResult* result,
    const signal::OpportunityIntent& intent,
    const RiskPolicySnapshot& policy
) {
    result->audit_trace.trace_id = intent.intent_id;
    result->audit_trace.intent_id = intent.intent_id;
    result->audit_trace.bundle_id = intent.bundle_id;
    result->audit_trace.policy_version = policy.policy_version;
    result->audit_trace.policy_hash = policy_hash_for(policy);
}

void add_step(
    RiskPipelineResult* result,
    std::string guard_name,
    bool pass,
    RiskDecisionType rejection,
    std::string reason = {}
) {
    RiskAuditStep step;
    step.guard_name = std::move(guard_name);
    step.pass = pass;
    step.rejection = rejection;
    step.reason = std::move(reason);
    result->audit_trace.steps.push_back(std::move(step));
}

[[nodiscard]] const state::MarketStateSnapshot* find_snapshot(
    const std::unordered_map<std::string, const state::MarketStateSnapshot*>&
        by_asset,
    const std::string& asset_id
) {
    const auto it = by_asset.find(asset_id);
    if (it == by_asset.end()) {
        return nullptr;
    }
    return it->second;
}

}  // namespace

RiskPipeline::RiskPipeline() : rate_limit_guard_(100) {}

void RiskPipeline::clear_duplicate_cache() {
    duplicate_guard_.clear();
}

RiskPipelineResult RiskPipeline::evaluate(
    const signal::OpportunityIntent& intent,
    const RiskEvaluationContext& context,
    ReservationBook* reservations
) {
    RiskPipelineResult result;
    result.result.intents_evaluated = 1;
    result.result.metrics.evaluate_count = 1;
    init_trace(&result, intent, context.policy);

    KillSwitchGuard kill_switch(context.policy.kill_switch_enabled);
    if (auto guard = kill_switch.check(intent, context.now_ns); !guard.pass) {
        add_step(&result, "KillSwitchGuard", false, guard.rejection, guard.reason);
        return reject_result(
            intent,
            reject_reason_for(guard.rejection),
            guard.reason,
            {},
            result
        );
    }
    add_step(&result, "KillSwitchGuard", true, RiskDecisionType::Pass);

    const auto validation =
        intent_validator_.validate(intent, context.now_ns);
    if (!validation.ok) {
        add_step(
            &result,
            "IntentValidator",
            false,
            RiskDecisionType::RejectInternalError,
            validation.detail
        );
        return reject_result(
            intent,
            validation.reject_reason,
            validation.detail,
            {},
            result
        );
    }
    add_step(&result, "IntentValidator", true, RiskDecisionType::Pass);

    const auto evidence = evidence_verifier_.verify(intent);
    if (!evidence.ok) {
        add_step(
            &result,
            "IntentEvidenceVerifier",
            false,
            RiskDecisionType::RejectInternalError,
            evidence.detail
        );
        return reject_result(
            intent,
            evidence.reject_reason,
            evidence.detail,
            {},
            result
        );
    }
    add_step(&result, "IntentEvidenceVerifier", true, RiskDecisionType::Pass);

    if (auto guard = expiry_guard_.check(intent, context.now_ns);
        !guard.pass) {
        add_step(&result, "IntentExpiryGuard", false, guard.rejection, guard.reason);
        return reject_result(
            intent,
            reject_reason_for(guard.rejection),
            guard.reason,
            {},
            result
        );
    }
    add_step(&result, "IntentExpiryGuard", true, RiskDecisionType::Pass);

    if (auto guard = duplicate_guard_.check(intent, context.now_ns);
        !guard.pass) {
        add_step(&result, "DuplicateIntentGuard", false, guard.rejection, guard.reason);
        return reject_result(
            intent,
            reject_reason_for(guard.rejection),
            guard.reason,
            {},
            result
        );
    }
    add_step(&result, "DuplicateIntentGuard", true, RiskDecisionType::Pass);

    rate_limit_guard_.set_max_per_second(
        context.policy.max_approvals_per_second
    );
    if (auto guard = rate_limit_guard_.check(intent, context.now_ns);
        !guard.pass) {
        add_step(&result, "RateLimitGuard", false, guard.rejection, guard.reason);
        return reject_result(
            intent,
            reject_reason_for(guard.rejection),
            guard.reason,
            {},
            result
        );
    }
    add_step(&result, "RateLimitGuard", true, RiskDecisionType::Pass);

    std::unordered_map<std::string, const state::MarketStateSnapshot*> by_asset;
    by_asset.reserve(context.latest_snapshots.size());
    for (const auto& snapshot : context.latest_snapshots) {
        by_asset.emplace(snapshot.entity_id, &snapshot);
    }

    for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
        const auto* snapshot = find_snapshot(by_asset, intent.legs[i].asset_id);
        if (auto guard = market_state_guard_.check(snapshot); !guard.pass) {
            add_step(&result, "MarketStateGuard", false, guard.rejection, guard.reason);
            return reject_result(
                intent,
                reject_reason_for(guard.rejection),
                guard.reason,
                {},
                result
            );
        }
        add_step(&result, "MarketStateGuard", true, RiskDecisionType::Pass);
        if (auto guard = snapshot_freshness_guard_.check(
                *snapshot,
                intent,
                context.policy,
                context.now_ns
            );
            !guard.pass) {
            add_step(
                &result,
                "SnapshotFreshnessGuard",
                false,
                guard.rejection,
                guard.reason
            );
            auto reason = reject_reason_for(guard.rejection);
            if (guard.rejection == RiskDecisionType::RejectStaleSnapshot &&
                guard.reason.find("drift") != std::string::npos) {
                reason = RiskRejectReason::SnapshotSkew;
            }
            return reject_result(intent, reason, guard.reason, {}, result);
        }
        add_step(&result, "SnapshotFreshnessGuard", true, RiskDecisionType::Pass);
    }

    result.cost_revalidated = true;
    result.result.metrics.vwap_recomputed = 1;
    auto cost = cost_revalidator_.revalidate(
        intent,
        context.latest_snapshots,
        context.policy
    );
    if (!cost.ok) {
        add_step(&result, "CostRevalidator", false, cost.rejection, cost.reason);
        return reject_result(
            intent,
            reject_reason_for(cost.rejection),
            cost.reason,
            cost,
            result
        );
    }
    add_step(&result, "CostRevalidator", true, RiskDecisionType::Pass);

    if (auto guard = edge_guard_.check(intent, cost, context.policy);
        !guard.pass) {
        add_step(&result, "EdgeGuard", false, guard.rejection, guard.reason);
        return reject_result(
            intent,
            reject_reason_for(guard.rejection),
            guard.reason,
            cost,
            result
        );
    }
    add_step(&result, "EdgeGuard", true, RiskDecisionType::Pass);

    if (auto guard = max_loss_guard_.check(intent, cost, context.policy);
        !guard.pass) {
        add_step(&result, "MaxLossGuard", false, guard.rejection, guard.reason);
        return reject_result(
            intent,
            reject_reason_for(guard.rejection),
            guard.reason,
            cost,
            result
        );
    }
    add_step(&result, "MaxLossGuard", true, RiskDecisionType::Pass);

    if (auto guard = exposure_guard_.check(
            context.ledger_snapshot,
            intent,
            cost,
            context.policy
        );
        !guard.pass) {
        add_step(&result, "ExposureGuard", false, guard.rejection, guard.reason);
        return reject_result(
            intent,
            reject_reason_for(guard.rejection),
            guard.reason,
            cost,
            result
        );
    }
    add_step(&result, "ExposureGuard", true, RiskDecisionType::Pass);

    if (auto guard = inventory_guard_.check(
            context.ledger_snapshot,
            intent,
            cost,
            context.policy
        );
        !guard.pass) {
        add_step(&result, "InventoryGuard", false, guard.rejection, guard.reason);
        return reject_result(
            intent,
            reject_reason_for(guard.rejection),
            guard.reason,
            cost,
            result
        );
    }
    add_step(&result, "InventoryGuard", true, RiskDecisionType::Pass);

    if (auto guard = partial_fill_guard_.check(
            intent,
            context.latest_snapshots,
            cost,
            context.policy
        );
        !guard.pass) {
        add_step(&result, "PartialFillGuard", false, guard.rejection, guard.reason);
        return reject_result(
            intent,
            reject_reason_for(guard.rejection),
            guard.reason,
            cost,
            result
        );
    }
    add_step(&result, "PartialFillGuard", true, RiskDecisionType::Pass);

    if (reservations == nullptr) {
        add_step(
            &result,
            "ReservationBook.try_reserve",
            false,
            RiskDecisionType::RejectInternalError,
            "missing reservation book"
        );
        return reject_result(
            intent,
            RiskRejectReason::MissingReservation,
            "missing reservation book",
            cost,
            result
        );
    }

    auto decision = make_approved_decision(
        context.policy.policy_version,
        policy_hash_for(context.policy)
    );

    result.reservation_attempted = true;
    result.reservation =
        reservations->try_reserve(intent, decision, context.now_ns);
    if (!result.reservation.ok) {
        add_step(
            &result,
            "ReservationBook.try_reserve",
            false,
            RiskDecisionType::RejectInternalError,
            result.reservation.detail
        );
        auto reason = result.reservation.reject_reason;
        if (reason == RiskRejectReason::DuplicateReservation) {
            reason = RiskRejectReason::DuplicateReservation;
        } else if (reason == RiskRejectReason::ExpiredIntent) {
            reason = RiskRejectReason::ExpiredIntent;
        } else {
            reason = RiskRejectReason::TotalExposureLimit;
        }
        return reject_result(
            intent,
            reason,
            result.reservation.detail,
            cost,
            result
        );
    }
    add_step(
        &result,
        "ReservationBook.try_reserve",
        true,
        RiskDecisionType::Pass
    );

    result.cost = cost;
    result.decision = decision;
    result.audit_trace.decision = decision;
    result.audit_trace.decision_id = decision.decision_id;
    result.approved_intent.intent = intent;
    result.approved_intent.decision = decision;
    result.approved_intent.reservation_id =
        std::to_string(result.reservation.reservation_id);
    result.approved_intent.approved_at_ns = context.now_ns;
    result.approved_intent.expires_at_ns = intent.expires_at_ns;
    result.result.intents_approved = 1;
    result.result.metrics.approve_count = 1;
    result.result.metrics.reservation_created = 1;
    result.output_hash = output_hash_for(
        intent,
        result.decision,
        result.cost,
        result.reservation.reservation_id
    );
    result.result.output_hash = result.output_hash;
    return result;
}

}  // namespace trading_engine::risk
