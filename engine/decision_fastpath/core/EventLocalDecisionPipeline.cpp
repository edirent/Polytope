#include "engine/decision_fastpath/core/EventLocalDecisionPipeline.h"

#include "engine/decision_fastpath/core/DifferentialVerifier.h"
#include "engine/decision_fastpath/kernel/FixedBuyBundleKernelScalar.h"
#include "engine/execution/plan/ExecutionPlanner.h"
#include "engine/order_decision/core/OrderDecisionEngine.h"
#include "engine/order_decision/public/ApprovedOrderDecisionEnvelope.h"
#include "engine/risk/core/RiskEngine.h"
#include "oracle/public/CandidateBundle.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <utility>

namespace trading_engine::decision_fastpath {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void mix_u64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        *hash ^= static_cast<std::uint8_t>((value >> shift) & 0xffU);
        *hash *= kFnvPrime;
    }
}

void mix_u8(std::uint64_t* hash, std::uint8_t value) noexcept {
    *hash ^= value;
    *hash *= kFnvPrime;
}

[[nodiscard]] std::uint64_t nonzero_hash(std::uint64_t hash) noexcept {
    return hash == 0 ? 1 : hash;
}

[[nodiscard]] FastPathResult fallback_result(
    const EventLocalInput& input,
    FastPathRejectReason reason,
    FastPathMode mode
) noexcept {
    FastPathResult result;
    result.mode = mode;
    result.fallback_required = true;
    result.generic_verification_required = true;
    result.reject_reason = reason;
    auto hash = kFnvOffset;
    mix_u64(&hash, input.dirty_asset_index);
    mix_u64(&hash, input.current_true_mask);
    mix_u64(&hash, input.current_false_mask);
    mix_u8(&hash, 1);
    mix_u8(&hash, static_cast<std::uint8_t>(reason));
    result.output_hash = nonzero_hash(hash);
    return result;
}

[[nodiscard]] bool explicit_fast_path_config(
    const EventLocalDecisionPipelineConfig& config
) noexcept {
    return config.fast_path.mode != FastPathMode::Disabled ||
           config.fast_path.enable_fixed_buy_kernel ||
           config.fast_path.enable_simd;
}

[[nodiscard]] FastPathConfig effective_fast_path_config(
    const EventLocalDecisionPipelineConfig& config
) noexcept {
    if (explicit_fast_path_config(config)) {
        return config.fast_path;
    }

    FastPathConfig out;
    out.mode = config.fast_path_enabled ? config.mode : FastPathMode::Disabled;
    out.enable_fixed_buy_kernel = config.fast_path_enabled;
    out.enable_simd = false;
    out.max_mismatches_before_disable =
        config.fast_path.max_mismatches_before_disable;
    if (config.periodic_sample_verification_enabled &&
        config.sample_verification_interval > 0) {
        out.sample_verify_rate =
            1.0 / static_cast<double>(config.sample_verification_interval);
    } else {
        out.sample_verify_rate = 0.0;
    }
    out.require_kernel_spec_hash_match =
        config.fast_path.require_kernel_spec_hash_match;
    out.require_artifact_hash_match =
        config.fast_path.require_artifact_hash_match;
    out.require_policy_hash_match =
        config.fast_path.require_policy_hash_match;
    return out;
}

void apply_rollout_policy(
    const FastPathConfig& fast_path,
    FastPathResult* result
) noexcept {
    result->mode = fast_path.mode;
    if (result->fallback_required) {
        result->publish_allowed = false;
        result->reservation_allowed = false;
        result->generic_verification_required = true;
        result->authoritative = false;
        result->sample_verification_required = false;
        return;
    }

    switch (fast_path.mode) {
        case FastPathMode::Disabled:
            result->publish_allowed = false;
            result->reservation_allowed = false;
            result->generic_verification_required = true;
            result->authoritative = false;
            break;
        case FastPathMode::ShadowCompare:
            result->publish_allowed = false;
            result->reservation_allowed = false;
            result->generic_verification_required = true;
            result->authoritative = false;
            break;
        case FastPathMode::VerifiedPaper:
            result->publish_allowed = result->produced_plan;
            result->reservation_allowed = false;
            result->generic_verification_required = true;
            result->authoritative = false;
            break;
        case FastPathMode::PaperAuthoritative:
            result->publish_allowed = result->produced_plan;
            result->reservation_allowed = result->produced_plan;
            result->generic_verification_required = false;
            result->authoritative = result->produced_plan;
            break;
    }

    result->sample_verification_required = false;
    if (fast_path.mode == FastPathMode::PaperAuthoritative &&
        result->produced_plan && fast_path.sample_verify_rate > 0.0) {
        const auto clamped = std::clamp(fast_path.sample_verify_rate, 0.0, 1.0);
        const auto bucket = result->output_hash % 1'000'000ULL;
        const auto threshold =
            static_cast<std::uint64_t>(std::ceil(clamped * 1'000'000.0));
        result->sample_verification_required = bucket < threshold;
        if (result->sample_verification_required) {
            result->generic_verification_required = true;
        }
    }
}

[[nodiscard]] std::uint64_t pipeline_output_hash(
    const EventLocalInput& input,
    const FastKernelResult& kernel_result,
    bool fallback_required,
    FastPathRejectReason reject_reason
) noexcept {
    auto hash = kFnvOffset;
    mix_u64(&hash, input.dirty_asset_index);
    mix_u64(&hash, input.current_true_mask);
    mix_u64(&hash, input.current_false_mask);
    mix_u64(&hash, kernel_result.output_hash);
    mix_u8(&hash, kernel_result.produced_plan ? 1U : 0U);
    mix_u8(&hash, fallback_required ? 1U : 0U);
    mix_u8(&hash, static_cast<std::uint8_t>(reject_reason));
    return nonzero_hash(hash);
}

void materialize_execution_strings(
    const trading_engine::order_decision::OrderDecisionLite& decision,
    trading_engine::signal::OpportunityIntent* intent
) {
    if (intent == nullptr) {
        return;
    }
    if (intent->idempotency_key.empty()) {
        intent->idempotency_key = std::to_string(intent->idempotency_hash);
    }
    for (std::uint16_t i = 0; i < intent->leg_count; ++i) {
        auto& leg = intent->legs[i];
        if (leg.market_id.empty()) {
            leg.market_id =
                "market_" + std::to_string(decision.legs[i].market_index);
        }
        if (leg.asset_id.empty()) {
            leg.asset_id = "asset_" + std::to_string(leg.asset_index);
        }
    }
}

[[nodiscard]] trading_engine::oracle::CandidateBundle bundle_from_spec(
    const FixedShapeKernelSpec& spec
) {
    trading_engine::oracle::CandidateBundle bundle;
    bundle.bundle_id = spec.bundle_id;
    bundle.guaranteed_payout_tick = spec.guaranteed_payout_tick;
    bundle.min_edge_tick = spec.min_unit_edge_tick;
    bundle.leg_count = spec.leg_count;
    for (std::uint16_t i = 0; i < spec.leg_count; ++i) {
        bundle.legs[i].market_id =
            "market_" + std::to_string(spec.market_indices[i]);
        bundle.legs[i].asset_id =
            "asset_" + std::to_string(spec.asset_indices[i]);
        bundle.legs[i].side = spec.sides[i];
        bundle.legs[i].quantity_lots = spec.ratio_qty_lots[i];
        bundle.legs[i].max_price_tick = 0;
    }
    return bundle;
}

[[nodiscard]] trading_engine::risk::RiskRejectReason risk_reason_for(
    trading_engine::order_decision::OrderDecisionType type
) noexcept {
    using trading_engine::order_decision::OrderDecisionType;
    using trading_engine::risk::RiskRejectReason;
    switch (type) {
        case OrderDecisionType::RejectLowEdge:
            return RiskRejectReason::LowTotalEdge;
        case OrderDecisionType::RejectNoDepth:
        case OrderDecisionType::RejectPartialFillRisk:
            return RiskRejectReason::PartialFillRisk;
        case OrderDecisionType::RejectExpiredIntent:
            return RiskRejectReason::ExpiredIntent;
        case OrderDecisionType::RejectRiskBudget:
            return RiskRejectReason::CostLimit;
        case OrderDecisionType::RejectUnsupportedSide:
        case OrderDecisionType::RejectInvalidBundle:
        case OrderDecisionType::RejectPriceProtection:
        case OrderDecisionType::NoTrade:
        case OrderDecisionType::PaperOrderDecision:
        default:
            return RiskRejectReason::InvalidIntent;
    }
}

[[nodiscard]] trading_engine::risk::RiskRejectReason risk_reason_for(
    const trading_engine::order_decision::OrderDecisionResult& result
) noexcept {
    if (result.reject_reason ==
        trading_engine::order_decision::OrderDecisionType::RejectLowEdge) {
        if (result.eval_stats.rejected_by_unit_edge > 0) {
            return trading_engine::risk::RiskRejectReason::LowUnitEdge;
        }
        if (result.eval_stats.rejected_by_total_edge > 0) {
            return trading_engine::risk::RiskRejectReason::LowTotalEdge;
        }
        if (result.eval_stats.rejected_by_bps > 0) {
            return trading_engine::risk::RiskRejectReason::LowEdgeBps;
        }
    }
    return risk_reason_for(result.reject_reason);
}

void complete_risk_and_plan(
    const EventLocalInput& input,
    const trading_engine::risk::RiskPolicySnapshot& risk_policy,
    FastPathResult* result
) {
    if (result == nullptr ||
        result->order_decision.type != trading_engine::order_decision::
            OrderDecisionType::PaperOrderDecision) {
        return;
    }

    materialize_execution_strings(result->order_decision, &result->intent);

    trading_engine::risk::RiskEngine risk_engine{
        trading_engine::risk::RiskRuntimeContext{
            .policy = &risk_policy
        }
    };
    trading_engine::risk::RiskInputView risk_input;
    risk_input.intent = &result->intent;
    risk_input.depth_views = input.depth_views;
    risk_input.depth_view_count = input.depth_view_count;
    risk_input.snapshot_version_hash = result->intent.snapshot_version_hash;
    risk_input.now_ns = input.now_ns;
    risk_input.policy = &risk_policy;
    risk_input.ledger = input.ledger;
    const auto risk_result = risk_engine.evaluate_decision(
        result->intent,
        result->order_decision,
        risk_input
    );
    result->decision = risk_result.decision;
    result->approved = risk_result.approved_intent;
    if (risk_result.reservation.reservation_id != 0) {
        result->approved.reservation_id_hash =
            risk_result.reservation.reservation_id;
    }

    if (!result->decision.approved() || !result->approved.valid()) {
        return;
    }

    const auto envelope =
        trading_engine::order_decision::make_approved_order_decision_envelope(
            result->approved,
            result->order_decision,
            input.now_ns
        );
    const trading_engine::execution::ExecutionPlanner planner;
    const auto plan_result = planner.build_plan(
        envelope,
        input.now_ns,
        trading_engine::execution::ExecutionConfig{}
    );
    if (plan_result.ok) {
        result->plan = plan_result.plan;
        result->produced_plan = true;
    }
}

void apply_fast_hashes(
    const DecisionPathSnapshot& snapshot,
    FastPathResult* result
) noexcept {
    result->fast_opportunity_hash = snapshot.opportunity_hash;
    result->fast_risk_hash = snapshot.risk_hash;
    result->fast_plan_hash = snapshot.plan_hash;
    result->fast_combined_hash = snapshot.combined_hash;
}

void apply_generic_hashes(
    const DecisionPathSnapshot& snapshot,
    FastPathResult* result
) noexcept {
    result->generic_opportunity_hash = snapshot.opportunity_hash;
    result->generic_risk_hash = snapshot.risk_hash;
    result->generic_plan_hash = snapshot.plan_hash;
    result->generic_combined_hash = snapshot.combined_hash;
    result->generic_output_hash = snapshot.combined_hash;
}

void apply_fast_hashes_from_result(FastPathResult* result) noexcept {
    apply_fast_hashes(snapshot_from_fast_result(*result), result);
}

void clear_non_authoritative_reservation(FastPathResult* result) {
    if (result == nullptr) {
        return;
    }
    result->reservation_allowed = false;
    result->approved.reservation_id.clear();
    result->approved.reservation_id_hash = 0;
}

[[nodiscard]] FastPathResult copy_kernel_result(
    const EventLocalInput& input,
    const FastKernelResult& kernel_result,
    const FastPathConfig& fast_path
) {
    FastPathResult result;
    result.mode = fast_path.mode;
    result.fallback_required = kernel_result.fallback_required;
    result.reject_reason = kernel_result.reject_reason;
    result.intent = kernel_result.intent;
    result.order_decision = kernel_result.order_decision;
    if (!result.fallback_required && kernel_result.produced_intent &&
        !kernel_result.produced_order_decision) {
        result.decision.status =
            trading_engine::risk::RiskDecisionStatus::Rejected;
        result.decision.reject_reason = kernel_result.risk_reject_reason;
        result.decision.intent_id = result.intent.intent_id;
        result.decision.bundle_id = result.intent.bundle_id;
        result.decision.idempotency_hash = result.intent.idempotency_hash;
        result.decision.oracle_artifact_hash =
            result.intent.oracle_artifact_hash;
        result.decision.constraint_hash = result.intent.constraint_hash;
        result.decision.bundle_hash = result.intent.bundle_hash;
        result.decision.snapshot_version_hash =
            result.intent.snapshot_version_hash;
        if (input.policy != nullptr) {
            result.decision.policy_version = input.policy->policy_version;
            result.decision.policy_hash = input.policy->policy_hash;
        }
    }

    if (!result.fallback_required && kernel_result.produced_order_decision &&
        input.policy != nullptr) {
        auto risk_policy = *input.policy;
        risk_policy.max_snapshot_skew_ns =
            std::numeric_limits<std::int64_t>::max();
        complete_risk_and_plan(input, risk_policy, &result);
    }

    result.output_hash = pipeline_output_hash(
        input,
        kernel_result,
        result.fallback_required,
        result.reject_reason
    );
    apply_rollout_policy(fast_path, &result);
    if (result.reservation_allowed && result.plan.reservation_id != 0) {
        result.approved.decision = result.decision;
        result.approved.reservation_id_hash = result.plan.reservation_id;
    } else {
        result.approved.reservation_id.clear();
        result.approved.reservation_id_hash = 0;
    }
    apply_fast_hashes_from_result(&result);
    return result;
}

[[nodiscard]] bool should_verify_generic(
    const FastPathResult& result
) noexcept {
    return result.generic_verification_required ||
           result.sample_verification_required;
}

[[nodiscard]] FastPathResult generic_order_decision_result(
    const EventLocalInput& input,
    const FixedShapeKernelSpec& spec,
    const trading_engine::signal::OpportunityIntent& source_intent,
    std::span<const trading_engine::state::MarketDepthView> depth_views,
    const trading_engine::risk::RiskPolicySnapshot& policy,
    FastPathMode mode
) {
    FastPathResult result;
    result.mode = mode;
    result.intent = source_intent;
    result.intent.status =
        trading_engine::signal::IntentStatus::PaperOpportunity;
    result.intent.reject_code =
        trading_engine::signal::IntentRejectCode::None;
    result.intent.leg_count = spec.leg_count;

    auto bundle = bundle_from_spec(spec);
    for (std::uint16_t i = 0; i < spec.leg_count; ++i) {
        auto& leg = result.intent.legs[i];
        leg.market_id = bundle.legs[i].market_id;
        leg.asset_id = bundle.legs[i].asset_id;
        leg.asset_index = spec.asset_indices[i];
        leg.side = spec.sides[i];
        leg.quantity_lots = spec.ratio_qty_lots[i];
    }

    trading_engine::order_decision::OrderDecisionConfig decision_config;
    decision_config.impl_mode =
        trading_engine::order_decision::OrderDecisionImplMode::Generic;
    const trading_engine::order_decision::OrderDecisionEngine engine{
        decision_config
    };
    const auto decision_result = engine.decide(
        result.intent,
        bundle,
        depth_views,
        policy,
        input.now_ns
    );

    if (!decision_result.ok) {
        result.intent.status =
            trading_engine::signal::IntentStatus::RejectedLowEdge;
        result.decision.status =
            trading_engine::risk::RiskDecisionStatus::Rejected;
        result.decision.reject_reason =
            risk_reason_for(decision_result);
        result.decision.intent_id = result.intent.intent_id;
        result.decision.bundle_id = result.intent.bundle_id;
        result.decision.idempotency_hash = result.intent.idempotency_hash;
        result.decision.oracle_artifact_hash =
            result.intent.oracle_artifact_hash;
        result.decision.constraint_hash = result.intent.constraint_hash;
        result.decision.bundle_hash = result.intent.bundle_hash;
        result.decision.snapshot_version_hash =
            result.intent.snapshot_version_hash;
        result.decision.policy_version = policy.policy_version;
        result.decision.policy_hash = policy.policy_hash;
        result.output_hash = nonzero_hash(result.intent.intent_id);
        apply_fast_hashes_from_result(&result);
        return result;
    }

    result.order_decision = decision_result.decision;
    for (std::uint16_t i = 0; i < result.order_decision.leg_count; ++i) {
        result.order_decision.legs[i].market_index = spec.market_indices[i];
        if (result.order_decision.legs[i].asset_index == 0) {
            result.order_decision.legs[i].asset_index = spec.asset_indices[i];
        }
    }
    result.order_decision.decision_hash =
        trading_engine::order_decision::compute_order_decision_hash(
            result.order_decision
        );
    result.order_decision.decision_id =
        result.order_decision.decision_hash;
    complete_risk_and_plan(input, policy, &result);
    result.output_hash = nonzero_hash(
        result.produced_plan ? result.plan.plan_id
                             : result.order_decision.decision_hash
    );
    apply_fast_hashes_from_result(&result);
    return result;
}

[[nodiscard]] DecisionPathSnapshot best_generic_snapshot(
    std::span<const FixedShapeKernelSpec> specs,
    std::span<const trading_engine::state::MarketDepthView> depth_views,
    const trading_engine::risk::RiskPolicySnapshot& policy,
    const trading_engine::risk::RiskLedgerSnapshot& ledger,
    std::uint64_t now_ns
) noexcept {
    DecisionPathSnapshot best;
    bool have_best = false;
    for (const auto& spec : specs) {
        const auto candidate = reference_generic_decision(
            spec,
            depth_views,
            policy,
            ledger,
            now_ns
        );
        if (candidate.fallback_required) {
            return candidate;
        }
        if (!have_best ||
            candidate.total_edge_tick > best.total_edge_tick ||
            (candidate.total_edge_tick == best.total_edge_tick &&
             candidate.bundle_id < best.bundle_id)) {
            best = candidate;
            have_best = true;
        }
    }
    return best;
}

void suppress_fast_output_for_mismatch(
    const DifferentialCompareResult& diff,
    FastPathRuntimeState* state,
    FastPathResult* result
) {
    ++state->mismatch_count;
    state->last_fast_hash = diff.fast_hash;
    state->last_generic_hash = diff.generic_hash;
    state->last_mismatch_reason = diff.first_mismatch;
    state->disabled = true;

    result->fallback_required = true;
    result->produced_plan = false;
    result->publish_allowed = false;
    result->reservation_allowed = false;
    result->authoritative = false;
    result->generic_comparison_performed = true;
    result->mismatch = true;
    result->fast_opportunity_hash = diff.fast_opportunity_hash;
    result->fast_risk_hash = diff.fast_risk_hash;
    result->fast_plan_hash = diff.fast_plan_hash;
    result->fast_combined_hash = diff.fast_combined_hash;
    result->generic_opportunity_hash = diff.generic_opportunity_hash;
    result->generic_risk_hash = diff.generic_risk_hash;
    result->generic_plan_hash = diff.generic_plan_hash;
    result->generic_combined_hash = diff.generic_combined_hash;
    result->generic_output_hash = diff.generic_combined_hash;
    result->mismatch_reason = diff.first_mismatch;
    result->reject_reason = FastPathRejectReason::RuntimeDisabled;
    result->output_hash = diff.generic_combined_hash;
    result->intent = {};
    result->decision = {};
    result->approved = {};
    result->plan = {};

    std::cerr << "fast_path mismatch: field=" << diff.first_mismatch
              << " fast_hash=" << diff.fast_hash
              << " generic_hash=" << diff.generic_hash << '\n';
}

}  // namespace

EventLocalDecisionPipeline::EventLocalDecisionPipeline(
    const FixedShapeKernelRegistry* registry,
    EventLocalDecisionPipelineConfig config
) : registry_(registry), config_(config) {}

FastPathResult EventLocalDecisionPipeline::process(
    const EventLocalInput& input
) const {
    const auto fast_path = effective_fast_path_config(config_);
    if (runtime_state_.disabled) {
        return fallback_result(
            input,
            FastPathRejectReason::RuntimeDisabled,
            fast_path.mode
        );
    }

    if (registry_ == nullptr || input.policy == nullptr) {
        return fallback_result(
            input,
            FastPathRejectReason::MissingPolicy,
            fast_path.mode
        );
    }

    if (fast_path.mode == FastPathMode::Disabled) {
        return fallback_result(
            input,
            FastPathRejectReason::RuntimeDisabled,
            fast_path.mode
        );
    }

    if (!fast_path.enable_fixed_buy_kernel ||
        !input.policy->risk_enabled ||
        input.policy->kill_switch_enabled) {
        return fallback_result(
            input,
            FastPathRejectReason::PolicyIncompatible,
            fast_path.mode
        );
    }

    const auto affected = registry_->specs_for_asset(input.dirty_asset_index);
    if (affected.empty()) {
        FastKernelResult empty;
        FastPathResult result;
        result.mode = fast_path.mode;
        result.output_hash = pipeline_output_hash(
            input,
            empty,
            false,
            FastPathRejectReason::None
        );
        result.generic_verification_required = false;
        apply_fast_hashes_from_result(&result);
        return result;
    }

    if (fast_path.require_policy_hash_match &&
        (config_.expected_policy_hash == 0 ||
         input.policy->policy_hash != config_.expected_policy_hash)) {
        return fallback_result(
            input,
            FastPathRejectReason::PolicyHashMismatch,
            fast_path.mode
        );
    }

    const trading_engine::risk::RiskLedgerSnapshot empty_ledger;
    const auto& ledger = input.ledger == nullptr ? empty_ledger : *input.ledger;
    const auto depth_count =
        input.depth_views == nullptr ? 0U : input.depth_view_count;

    const FixedBuyBundleKernelScalar kernel;
    FastPathScratch scratch;

    FastPathResult best;
    bool have_best = false;

    for (const auto& spec : affected) {
        if (fast_path.require_kernel_spec_hash_match &&
            spec.kernel_spec_hash != hash_fixed_shape_kernel_spec(spec)) {
            return fallback_result(
                input,
                FastPathRejectReason::KernelSpecHashMismatch,
                fast_path.mode
            );
        }
        if (fast_path.require_artifact_hash_match &&
            (config_.expected_artifact_hash == 0 ||
             spec.artifact_hash != config_.expected_artifact_hash)) {
            return fallback_result(
                input,
                FastPathRejectReason::ArtifactHashMismatch,
                fast_path.mode
            );
        }
        if (fast_path.require_artifact_hash_match &&
            (config_.expected_constraint_hash == 0 ||
             spec.constraint_hash != config_.expected_constraint_hash)) {
            return fallback_result(
                input,
                FastPathRejectReason::ArtifactHashMismatch,
                fast_path.mode
            );
        }
        if (spec.leg_count > config_.max_supported_legs) {
            return fallback_result(
                input,
                FastPathRejectReason::TooManyLegs,
                fast_path.mode
            );
        }

        const auto kernel_result = kernel.run(
            spec,
            input.depth_views,
            static_cast<std::uint16_t>(depth_count),
            *input.policy,
            ledger,
            input.now_ns,
            &scratch
        );
        auto candidate = copy_kernel_result(
            input,
            kernel_result,
            fast_path
        );

        if (candidate.fallback_required) {
            return candidate;
        }

        if (!have_best ||
            candidate.intent.total_edge_tick > best.intent.total_edge_tick ||
            (candidate.intent.total_edge_tick == best.intent.total_edge_tick &&
             candidate.intent.bundle_id < best.intent.bundle_id)) {
            best = std::move(candidate);
            have_best = true;
        }
    }

    if (have_best) {
        if (should_verify_generic(best)) {
            auto generic_policy = *input.policy;
            generic_policy.max_snapshot_skew_ns =
                std::numeric_limits<std::int64_t>::max();
            const FixedShapeKernelSpec* generic_spec = nullptr;
            for (const auto& spec : affected) {
                if (spec.bundle_id == best.intent.bundle_id) {
                    generic_spec = &spec;
                    break;
                }
            }
            const auto generic_result = generic_spec == nullptr
                ? FastPathResult{}
                : generic_order_decision_result(
                    input,
                    *generic_spec,
                    best.intent,
                    std::span<const trading_engine::state::MarketDepthView>{
                        input.depth_views,
                        depth_count
                    },
                    generic_policy,
                    fast_path.mode
                );
            const auto generic = generic_spec == nullptr
                ? DecisionPathSnapshot{}
                : snapshot_from_fast_result(generic_result);
            const auto diff = compare_decision_snapshots(
                snapshot_from_fast_result(best),
                generic
            );
            best.generic_comparison_performed = true;
            apply_generic_hashes(generic, &best);
            if (!diff.match) {
                if (fast_path.mode == FastPathMode::PaperAuthoritative) {
                    suppress_fast_output_for_mismatch(
                        diff,
                        &runtime_state_,
                        &best
                    );
                    if (runtime_state_.mismatch_count <
                        fast_path.max_mismatches_before_disable) {
                        runtime_state_.disabled = false;
                    }
                } else {
                    best = generic_result;
                    best.generic_comparison_performed = true;
                    best.mismatch = true;
                    best.fast_opportunity_hash = diff.fast_opportunity_hash;
                    best.fast_risk_hash = diff.fast_risk_hash;
                    best.fast_plan_hash = diff.fast_plan_hash;
                    best.fast_combined_hash = diff.fast_combined_hash;
                    best.generic_opportunity_hash = diff.generic_opportunity_hash;
                    best.generic_risk_hash = diff.generic_risk_hash;
                    best.generic_plan_hash = diff.generic_plan_hash;
                    best.generic_combined_hash = diff.generic_combined_hash;
                    best.generic_output_hash = diff.generic_combined_hash;
                    best.mismatch_reason = diff.first_mismatch;
                    best.publish_allowed =
                        fast_path.mode == FastPathMode::VerifiedPaper &&
                        best.produced_plan;
                    clear_non_authoritative_reservation(&best);
                    best.authoritative = false;
                }
            } else if (fast_path.mode == FastPathMode::ShadowCompare) {
                best = generic_result;
                best.generic_comparison_performed = true;
                best.fast_opportunity_hash = diff.fast_opportunity_hash;
                best.fast_risk_hash = diff.fast_risk_hash;
                best.fast_plan_hash = diff.fast_plan_hash;
                best.fast_combined_hash = diff.fast_combined_hash;
                best.generic_opportunity_hash = diff.generic_opportunity_hash;
                best.generic_risk_hash = diff.generic_risk_hash;
                best.generic_plan_hash = diff.generic_plan_hash;
                best.generic_combined_hash = diff.generic_combined_hash;
                best.generic_output_hash = diff.generic_combined_hash;
                best.publish_allowed = false;
                clear_non_authoritative_reservation(&best);
                best.authoritative = false;
                best.generic_verification_required = true;
            }
        }
        return best;
    }

    FastKernelResult empty;
    FastPathResult result;
    result.mode = fast_path.mode;
    result.output_hash = pipeline_output_hash(
        input,
        empty,
        false,
        FastPathRejectReason::None
    );
    apply_rollout_policy(fast_path, &result);
    apply_fast_hashes_from_result(&result);
    return result;
}

const FastPathRuntimeState& EventLocalDecisionPipeline::runtime_state()
    const noexcept {
    return runtime_state_;
}

void EventLocalDecisionPipeline::reset_runtime_state() const noexcept {
    runtime_state_ = FastPathRuntimeState{};
}

}  // namespace trading_engine::decision_fastpath
