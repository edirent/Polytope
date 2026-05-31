#include "engine/risk/core/RiskPipeline.h"

#include "engine/risk/public/RiskDecision.h"
#include "engine/state/view/MarketDepthView.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

namespace trading_engine::risk {

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t elapsed_ns(Clock::time_point start) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start
        ).count()
    );
}

[[nodiscard]] std::uint64_t stage_sum_ns(
    const RiskStageTimings& timings
) noexcept {
    return timings.kill_switch_guard_ns + timings.intent_validator_ns +
           timings.evidence_verifier_ns + timings.expiry_guard_ns +
           timings.duplicate_guard_ns + timings.rate_limit_guard_ns +
           timings.market_state_guard_ns +
           timings.snapshot_freshness_guard_ns +
           timings.cost_revalidator_ns + timings.edge_guard_ns +
           timings.max_loss_guard_ns +
           timings.exposure_guard_ns + timings.inventory_guard_ns +
           timings.partial_fill_guard_ns + timings.reservation_book_ns +
           timings.audit_trace_ns + timings.risk_decision_build_ns +
           timings.publisher_ns + timings.metrics_ns;
}

void finalize_stage_timings(
    RiskPipelineResult* result,
    Clock::time_point total_start
) {
    if (result == nullptr) {
        return;
    }

    auto& timings = result->result.stage_timings;
    timings.total_ns = elapsed_ns(total_start);
    timings.stage_sum_ns = stage_sum_ns(timings);
    timings.unattributed_ns =
        timings.total_ns > timings.stage_sum_ns
            ? timings.total_ns - timings.stage_sum_ns
            : 0;
}

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

[[nodiscard]] bool should_track_reservation_ledger_detail(
    const RiskPolicySnapshot& policy
) noexcept {
    return policy.max_total_exposure_tick > 0 ||
           policy.max_single_market_exposure_tick > 0 ||
           policy.max_inventory_lots_per_asset > 0;
}

void publish_decision_if_needed(
    RiskPipelineResult* result,
    IRiskDecisionPublisher* publisher
) {
    if (result == nullptr || publisher == nullptr) {
        return;
    }

    const auto start = Clock::now();
    publisher->publish_result(*result);
    result->result.stage_timings.publisher_ns += elapsed_ns(start);
}

void attach_decision_evidence(
    RiskDecision* decision,
    const signal::OpportunityIntent& intent
) noexcept {
    if (decision == nullptr) {
        return;
    }

    decision->intent_id = intent.intent_id;
    decision->bundle_id = intent.bundle_id;
    decision->idempotency_hash = intent.idempotency_hash;
    decision->oracle_artifact_hash = intent.oracle_artifact_hash;
    decision->constraint_hash = intent.constraint_hash;
    decision->bundle_hash = intent.bundle_hash;
    decision->snapshot_version_hash = intent.snapshot_version_hash;
}

[[nodiscard]] RiskPipelineResult reject_result(
    const signal::OpportunityIntent& intent,
    RiskRejectReason reason,
    std::string detail,
    const CostRevalidationResult& cost,
    RiskPipelineResult result,
    Clock::time_point total_start,
    IRiskDecisionPublisher* decision_publisher
) {
    auto stage_start = Clock::now();
    record_reject_reason(reason, &result.result);
    result.result.stage_timings.metrics_ns += elapsed_ns(stage_start);

    stage_start = Clock::now();
    result.decision = make_rejected_decision(reason, std::move(detail));
    attach_decision_evidence(&result.decision, intent);
    result.cost = cost;
    result.output_hash = output_hash_for(intent, result.decision, result.cost, 0);
    result.decision.decision_id = result.output_hash;
    result.audit_trace.decision = result.decision;
    result.audit_trace.decision_id = result.decision.decision_id;
    result.audit_trace.lite.decision_id = result.decision.decision_id;
    result.result.output_hash = result.output_hash;
    result.result.stage_timings.risk_decision_build_ns += elapsed_ns(stage_start);

    publish_decision_if_needed(&result, decision_publisher);
    finalize_stage_timings(&result, total_start);
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
    result->audit_trace.lite.step_count = 0;
}

void add_step(
    RiskPipelineResult* result,
    RiskAuditStepCode step_code,
    bool pass,
    RiskDecisionType rejection,
    std::string_view reason = {},
    bool full_trace = false
) {
    auto& lite = result->audit_trace.lite;
    if (lite.step_count < lite.steps.size()) {
        auto& step = lite.steps[lite.step_count++];
        step.step = step_code;
        step.pass = pass;
        step.rejection = rejection;
        step.detail_code = static_cast<std::uint64_t>(rejection);
    }

    if (full_trace) {
        RiskAuditStep step;
        step.guard_name = risk_audit_step_name(step_code);
        step.pass = pass;
        step.rejection = rejection;
        step.reason = std::string{reason};
        result->audit_trace.steps.push_back(std::move(step));
    }
}

void timed_add_step(
    RiskPipelineResult* result,
    RiskAuditStepCode step_code,
    bool pass,
    RiskDecisionType rejection,
    std::string_view reason = {},
    bool full_trace = false
) {
    const auto start = Clock::now();
    add_step(
        result,
        step_code,
        pass,
        rejection,
        std::move(reason),
        full_trace
    );
    result->result.stage_timings.audit_trace_ns += elapsed_ns(start);
}

[[nodiscard]] const state::MarketStateSnapshot* find_snapshot(
    const RiskInputView& input,
    const std::string& asset_id
) {
    if (input.snapshots == nullptr) {
        return nullptr;
    }
    for (std::uint16_t i = 0; i < input.snapshot_count; ++i) {
        if (input.snapshots[i].entity_id == asset_id) {
            return &input.snapshots[i];
        }
    }
    return nullptr;
}

[[nodiscard]] const state::MarketDepthView* find_depth_view(
    const RiskInputView& input,
    std::uint32_t asset_index
) {
    if (input.depth_views == nullptr) {
        return nullptr;
    }
    for (std::uint16_t i = 0; i < input.depth_view_count; ++i) {
        if (input.depth_views[i].asset_index == asset_index) {
            return &input.depth_views[i];
        }
    }
    return nullptr;
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
    RiskInputView input;
    input.intent = &intent;
    input.snapshots = context.latest_snapshots.data();
    input.snapshot_count = static_cast<std::uint16_t>(
        context.latest_snapshots.size()
    );
    input.snapshot_version_hash = context.latest_snapshot_version_hash;
    input.now_ns = context.now_ns;
    input.policy = &context.policy;
    input.ledger = &context.ledger_snapshot;

    return evaluate_view(
        input,
        reservations,
        context.decision_publisher,
        context.enable_full_audit_trace
    );
}

RiskPipelineResult RiskPipeline::evaluate_view(
    const RiskInputView& input,
    ReservationBook* reservations,
    IRiskDecisionPublisher* decision_publisher,
    bool enable_full_audit_trace
) {
    const auto total_start = Clock::now();
    scratch_.reset();
    RiskPipelineResult result;
    const bool full_audit_trace = enable_full_audit_trace;
    if (input.intent == nullptr) {
        result.result.intents_evaluated = 1;
        result.result.metrics.evaluate_count = 1;
        result.decision = make_rejected_decision(
            RiskRejectReason::InvalidIntent,
            "missing risk input intent"
        );
        result.audit_trace.decision = result.decision;
        result.audit_trace.decision_id = result.decision.decision_id;
        record_reject_reason(RiskRejectReason::InvalidIntent, &result.result);
        finalize_stage_timings(&result, total_start);
        return result;
    }

    const auto& intent = *input.intent;
    RiskPolicySnapshot default_policy;
    const auto& policy =
        input.policy != nullptr ? *input.policy : default_policy;
    auto stage_start = Clock::now();
    result.result.intents_evaluated = 1;
    result.result.metrics.evaluate_count = 1;
    result.result.stage_timings.metrics_ns += elapsed_ns(stage_start);

    stage_start = Clock::now();
    init_trace(&result, intent, policy);
    result.result.stage_timings.audit_trace_ns += elapsed_ns(stage_start);

    if (input.policy == nullptr || input.ledger == nullptr ||
        (input.snapshots == nullptr && input.snapshot_count > 0) ||
        (input.depth_views == nullptr && input.depth_view_count > 0)) {
        timed_add_step(
            &result,
            RiskAuditStepCode::IntentValidator,
            false,
            RiskDecisionType::RejectInternalError,
            "invalid risk input view",
            full_audit_trace
        );
        return reject_result(
            intent,
            RiskRejectReason::InvalidIntent,
            "invalid risk input view",
            {},
            result,
            total_start,
            decision_publisher
        );
    }

    stage_start = Clock::now();
    const auto& ledger = *input.ledger;

    KillSwitchGuard kill_switch(policy.kill_switch_enabled);
    auto kill_switch_result = kill_switch.check(intent, input.now_ns);
    result.result.stage_timings.kill_switch_guard_ns += elapsed_ns(stage_start);
    if (!kill_switch_result.pass) {
        timed_add_step(
            &result,
            RiskAuditStepCode::KillSwitchGuard,
            false,
            kill_switch_result.rejection,
            kill_switch_result.reason,
            full_audit_trace
        );
        return reject_result(
            intent,
            reject_reason_for(kill_switch_result.rejection),
            kill_switch_result.reason,
            {},
            result,
            total_start,
            decision_publisher
        );
    }
    timed_add_step(
        &result,
        RiskAuditStepCode::KillSwitchGuard,
        true,
        RiskDecisionType::Pass,
        {},
        full_audit_trace
    );

    stage_start = Clock::now();
    const auto validation =
        intent_validator_.validate(intent, input.now_ns);
    result.result.stage_timings.intent_validator_ns += elapsed_ns(stage_start);
    if (!validation.ok) {
        timed_add_step(
            &result,
            RiskAuditStepCode::IntentValidator,
            false,
            RiskDecisionType::RejectInternalError,
            validation.detail,
            full_audit_trace
        );
        return reject_result(
            intent,
            validation.reject_reason,
            validation.detail,
            {},
            result,
            total_start,
            decision_publisher
        );
    }
    timed_add_step(
        &result,
        RiskAuditStepCode::IntentValidator,
        true,
        RiskDecisionType::Pass,
        {},
        full_audit_trace
    );

    stage_start = Clock::now();
    const auto evidence = evidence_verifier_.verify(intent);
    result.result.stage_timings.evidence_verifier_ns += elapsed_ns(stage_start);
    if (!evidence.ok) {
        timed_add_step(
            &result,
            RiskAuditStepCode::EvidenceVerifier,
            false,
            RiskDecisionType::RejectInternalError,
            evidence.detail,
            full_audit_trace
        );
        return reject_result(
            intent,
            evidence.reject_reason,
            evidence.detail,
            {},
            result,
            total_start,
            decision_publisher
        );
    }
    timed_add_step(
        &result,
        RiskAuditStepCode::EvidenceVerifier,
        true,
        RiskDecisionType::Pass,
        {},
        full_audit_trace
    );

    stage_start = Clock::now();
    auto expiry_result = expiry_guard_.check(intent, input.now_ns);
    result.result.stage_timings.expiry_guard_ns += elapsed_ns(stage_start);
    if (!expiry_result.pass) {
        timed_add_step(
            &result,
            RiskAuditStepCode::ExpiryGuard,
            false,
            expiry_result.rejection,
            expiry_result.reason,
            full_audit_trace
        );
        return reject_result(
            intent,
            reject_reason_for(expiry_result.rejection),
            expiry_result.reason,
            {},
            result,
            total_start,
            decision_publisher
        );
    }
    timed_add_step(
        &result,
        RiskAuditStepCode::ExpiryGuard,
        true,
        RiskDecisionType::Pass,
        {},
        full_audit_trace
    );

    stage_start = Clock::now();
    auto duplicate_result = duplicate_guard_.check(intent, input.now_ns);
    result.result.stage_timings.duplicate_guard_ns += elapsed_ns(stage_start);
    if (!duplicate_result.pass) {
        timed_add_step(
            &result,
            RiskAuditStepCode::DuplicateGuard,
            false,
            duplicate_result.rejection,
            duplicate_result.reason,
            full_audit_trace
        );
        return reject_result(
            intent,
            reject_reason_for(duplicate_result.rejection),
            duplicate_result.reason,
            {},
            result,
            total_start,
            decision_publisher
        );
    }
    timed_add_step(
        &result,
        RiskAuditStepCode::DuplicateGuard,
        true,
        RiskDecisionType::Pass,
        {},
        full_audit_trace
    );

    stage_start = Clock::now();
    rate_limit_guard_.set_max_per_second(
        policy.max_approvals_per_second
    );
    auto rate_limit_result = rate_limit_guard_.check(intent, input.now_ns);
    result.result.stage_timings.rate_limit_guard_ns += elapsed_ns(stage_start);
    if (!rate_limit_result.pass) {
        timed_add_step(
            &result,
            RiskAuditStepCode::RateLimitGuard,
            false,
            rate_limit_result.rejection,
            rate_limit_result.reason,
            full_audit_trace
        );
        return reject_result(
            intent,
            reject_reason_for(rate_limit_result.rejection),
            rate_limit_result.reason,
            {},
            result,
            total_start,
            decision_publisher
        );
    }
    timed_add_step(
        &result,
        RiskAuditStepCode::RateLimitGuard,
        true,
        RiskDecisionType::Pass,
        {},
        full_audit_trace
    );

    for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
        const auto* depth_view = find_depth_view(
            input,
            intent.legs[i].asset_index
        );
        const auto* snapshot = depth_view == nullptr
            ? find_snapshot(input, intent.legs[i].asset_id)
            : nullptr;
        stage_start = Clock::now();
        auto market_state_result = depth_view != nullptr
            ? market_state_guard_.check(depth_view)
            : market_state_guard_.check(snapshot);
        result.result.stage_timings.market_state_guard_ns +=
            elapsed_ns(stage_start);
        if (!market_state_result.pass) {
            timed_add_step(
                &result,
                RiskAuditStepCode::MarketStateGuard,
                false,
                market_state_result.rejection,
                market_state_result.reason,
                full_audit_trace
            );
            return reject_result(
                intent,
                reject_reason_for(market_state_result.rejection),
                market_state_result.reason,
                {},
                result,
                total_start,
                decision_publisher
            );
        }
        timed_add_step(
            &result,
            RiskAuditStepCode::MarketStateGuard,
            true,
            RiskDecisionType::Pass,
            {},
            full_audit_trace
        );
        stage_start = Clock::now();
        auto freshness_result = depth_view != nullptr
            ? snapshot_freshness_guard_.check(
                  *depth_view,
                  intent,
                  policy,
                  input.now_ns
              )
            : snapshot_freshness_guard_.check(
                  *snapshot,
                  intent,
                  policy,
                  input.now_ns
              );
        result.result.stage_timings.snapshot_freshness_guard_ns +=
            elapsed_ns(stage_start);
        if (!freshness_result.pass) {
            timed_add_step(
                &result,
                RiskAuditStepCode::SnapshotFreshnessGuard,
                false,
                freshness_result.rejection,
                freshness_result.reason,
                full_audit_trace
            );
            auto reason = reject_reason_for(freshness_result.rejection);
            if (freshness_result.rejection ==
                    RiskDecisionType::RejectStaleSnapshot &&
                freshness_result.reason.find("drift") != std::string::npos) {
                reason = RiskRejectReason::SnapshotSkew;
            }
            return reject_result(
                intent,
                reason,
                freshness_result.reason,
                {},
                result,
                total_start,
                decision_publisher
            );
        }
        timed_add_step(
            &result,
            RiskAuditStepCode::SnapshotFreshnessGuard,
            true,
            RiskDecisionType::Pass,
            {},
            full_audit_trace
        );
    }

    stage_start = Clock::now();
    auto cost = input.depth_views != nullptr && input.depth_view_count > 0
        ? cost_revalidator_.revalidate(
              intent,
              input.depth_views,
              input.depth_view_count,
              policy,
              input.now_ns,
              input.snapshot_version_hash,
              &result.result.stage_timings
          )
        : cost_revalidator_.revalidate(
              intent,
              input.snapshots,
              input.snapshot_count,
              policy,
              input.now_ns,
              input.snapshot_version_hash,
              &result.result.stage_timings
          );
    const auto measured_cost_revalidator_ns = elapsed_ns(stage_start);
    if (result.result.stage_timings.cost_revalidator_ns == 0) {
        result.result.stage_timings.cost_revalidator_ns +=
            measured_cost_revalidator_ns;
    }
    result.cost_revalidated = true;
    stage_start = Clock::now();
    if (cost.vwap_mode == RiskVWAPMode::ReuseSignalSnapshot) {
        result.result.metrics.vwap_reused_signal_snapshot = 1;
        result.result.metrics.vwap_reused_signal_cost = 1;
        result.result.metrics.snapshot_fast_path = 1;
    } else if (cost.vwap_mode == RiskVWAPMode::ReusedSignalCost) {
        result.result.metrics.vwap_reused_signal_cost = 1;
        result.result.metrics.snapshot_fast_path = 1;
    } else {
        result.result.metrics.vwap_recomputed = 1;
        result.result.metrics.snapshot_requery = 1;
    }
    result.result.stage_timings.metrics_ns += elapsed_ns(stage_start);
    if (!cost.ok) {
        timed_add_step(
            &result,
            RiskAuditStepCode::CostRevalidator,
            false,
            cost.rejection,
            cost.reason,
            full_audit_trace
        );
        return reject_result(
            intent,
            reject_reason_for(cost.rejection),
            cost.reason,
            cost,
            result,
            total_start,
            decision_publisher
        );
    }
    timed_add_step(
        &result,
        RiskAuditStepCode::CostRevalidator,
        true,
        RiskDecisionType::Pass,
        {},
        full_audit_trace
    );

    stage_start = Clock::now();
    auto edge_result = edge_guard_.check(intent, cost, policy);
    result.result.stage_timings.edge_guard_ns += elapsed_ns(stage_start);
    if (!edge_result.pass) {
        timed_add_step(
            &result,
            RiskAuditStepCode::EdgeGuard,
            false,
            edge_result.rejection,
            edge_result.reason,
            full_audit_trace
        );
        return reject_result(
            intent,
            reject_reason_for(edge_result.rejection),
            edge_result.reason,
            cost,
            result,
            total_start,
            decision_publisher
        );
    }
    timed_add_step(
        &result,
        RiskAuditStepCode::EdgeGuard,
        true,
        RiskDecisionType::Pass,
        {},
        full_audit_trace
    );

    stage_start = Clock::now();
    auto max_loss_result = max_loss_guard_.check(intent, cost, policy);
    result.result.stage_timings.max_loss_guard_ns += elapsed_ns(stage_start);
    if (!max_loss_result.pass) {
        timed_add_step(
            &result,
            RiskAuditStepCode::MaxLossGuard,
            false,
            max_loss_result.rejection,
            max_loss_result.reason,
            full_audit_trace
        );
        return reject_result(
            intent,
            reject_reason_for(max_loss_result.rejection),
            max_loss_result.reason,
            cost,
            result,
            total_start,
            decision_publisher
        );
    }
    timed_add_step(
        &result,
        RiskAuditStepCode::MaxLossGuard,
        true,
        RiskDecisionType::Pass,
        {},
        full_audit_trace
    );

    stage_start = Clock::now();
    auto exposure_result = exposure_guard_.check(
        ledger,
        intent,
        cost,
        policy
    );
    result.result.stage_timings.exposure_guard_ns += elapsed_ns(stage_start);
    if (!exposure_result.pass) {
        timed_add_step(
            &result,
            RiskAuditStepCode::ExposureGuard,
            false,
            exposure_result.rejection,
            exposure_result.reason,
            full_audit_trace
        );
        return reject_result(
            intent,
            reject_reason_for(exposure_result.rejection),
            exposure_result.reason,
            cost,
            result,
            total_start,
            decision_publisher
        );
    }
    timed_add_step(
        &result,
        RiskAuditStepCode::ExposureGuard,
        true,
        RiskDecisionType::Pass,
        {},
        full_audit_trace
    );

    stage_start = Clock::now();
    auto inventory_result = inventory_guard_.check(
        ledger,
        intent,
        cost,
        policy
    );
    result.result.stage_timings.inventory_guard_ns += elapsed_ns(stage_start);
    if (!inventory_result.pass) {
        timed_add_step(
            &result,
            RiskAuditStepCode::InventoryGuard,
            false,
            inventory_result.rejection,
            inventory_result.reason,
            full_audit_trace
        );
        return reject_result(
            intent,
            reject_reason_for(inventory_result.rejection),
            inventory_result.reason,
            cost,
            result,
            total_start,
            decision_publisher
        );
    }
    timed_add_step(
        &result,
        RiskAuditStepCode::InventoryGuard,
        true,
        RiskDecisionType::Pass,
        {},
        full_audit_trace
    );

    stage_start = Clock::now();
    auto partial_fill_result = partial_fill_guard_.check(
        intent,
        cost,
        policy
    );
    result.result.stage_timings.partial_fill_guard_ns += elapsed_ns(stage_start);
    if (!partial_fill_result.pass) {
        timed_add_step(
            &result,
            RiskAuditStepCode::PartialFillGuard,
            false,
            partial_fill_result.rejection,
            partial_fill_result.reason,
            full_audit_trace
        );
        return reject_result(
            intent,
            reject_reason_for(partial_fill_result.rejection),
            partial_fill_result.reason,
            cost,
            result,
            total_start,
            decision_publisher
        );
    }
    timed_add_step(
        &result,
        RiskAuditStepCode::PartialFillGuard,
        true,
        RiskDecisionType::Pass,
        {},
        full_audit_trace
    );

    if (reservations == nullptr) {
        timed_add_step(
            &result,
            RiskAuditStepCode::ReservationBook,
            false,
            RiskDecisionType::RejectInternalError,
            "missing reservation book",
            full_audit_trace
        );
        return reject_result(
            intent,
            RiskRejectReason::MissingReservation,
            "missing reservation book",
            cost,
            result,
            total_start,
            decision_publisher
        );
    }

    stage_start = Clock::now();
    auto decision = make_approved_decision(
        policy.policy_version,
        policy_hash_for(policy)
    );
    attach_decision_evidence(&decision, intent);
    result.result.stage_timings.risk_decision_build_ns +=
        elapsed_ns(stage_start);

    result.reservation_attempted = true;
    stage_start = Clock::now();
    result.reservation = reservations->try_reserve(
        intent,
        decision,
        input.now_ns,
        should_track_reservation_ledger_detail(policy)
    );
    result.result.stage_timings.reservation_book_ns += elapsed_ns(stage_start);
    if (!result.reservation.ok) {
        timed_add_step(
            &result,
            RiskAuditStepCode::ReservationBook,
            false,
            RiskDecisionType::RejectInternalError,
            result.reservation.detail,
            full_audit_trace
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
            result,
            total_start,
            decision_publisher
        );
    }
    timed_add_step(
        &result,
        RiskAuditStepCode::ReservationBook,
        true,
        RiskDecisionType::Pass,
        {},
        full_audit_trace
    );

    stage_start = Clock::now();
    result.cost = cost;
    result.decision = decision;
    result.output_hash = output_hash_for(
        intent,
        result.decision,
        result.cost,
        result.reservation.reservation_id
    );
    result.decision.decision_id = result.output_hash;
    result.audit_trace.decision = result.decision;
    result.audit_trace.decision_id = result.decision.decision_id;
    result.audit_trace.lite.decision_id = result.decision.decision_id;
    result.approved_intent.intent = intent;
    result.approved_intent.decision = result.decision;
    result.approved_intent.reservation_id =
        std::to_string(result.reservation.reservation_id);
    result.approved_intent.approved_at_ns = input.now_ns;
    result.approved_intent.expires_at_ns = intent.expires_at_ns;
    result.result.output_hash = result.output_hash;
    result.result.stage_timings.risk_decision_build_ns +=
        elapsed_ns(stage_start);

    stage_start = Clock::now();
    result.result.intents_approved = 1;
    result.result.metrics.approve_count = 1;
    result.result.metrics.reservation_created = 1;
    result.result.stage_timings.metrics_ns += elapsed_ns(stage_start);

    publish_decision_if_needed(&result, decision_publisher);
    finalize_stage_timings(&result, total_start);
    return result;
}

}  // namespace trading_engine::risk
