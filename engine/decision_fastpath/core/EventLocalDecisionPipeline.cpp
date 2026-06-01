#include "engine/decision_fastpath/core/EventLocalDecisionPipeline.h"

#include "engine/decision_fastpath/core/DifferentialVerifier.h"
#include "engine/decision_fastpath/kernel/FixedBuyBundleKernelScalar.h"

#include <algorithm>
#include <cmath>
#include <iostream>
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

[[nodiscard]] FastPathResult copy_kernel_result(
    const EventLocalInput& input,
    const FastKernelResult& kernel_result,
    const FastPathConfig& fast_path
) {
    FastPathResult result;
    result.mode = fast_path.mode;
    result.produced_plan = kernel_result.produced_plan;
    result.fallback_required = kernel_result.fallback_required;
    result.reject_reason = kernel_result.reject_reason;
    result.intent = kernel_result.intent;
    result.decision = kernel_result.decision;
    result.approved = kernel_result.approved;
    result.plan = kernel_result.plan;

    if (result.produced_plan) {
        result.approved.approved_at_ns = input.now_ns;
        result.approved.expires_at_ns = result.intent.expires_at_ns;
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
            const auto generic = best_generic_snapshot(
                affected,
                std::span<const trading_engine::state::MarketDepthView>{
                    input.depth_views,
                    depth_count
                },
                *input.policy,
                ledger,
                input.now_ns
            );
            const auto diff = compare_decision_snapshots(
                snapshot_from_fast_result(best),
                generic
            );
            best.generic_comparison_performed = true;
            apply_generic_hashes(generic, &best);
            if (!diff.match) {
                suppress_fast_output_for_mismatch(
                    diff,
                    &runtime_state_,
                    &best
                );
                if (runtime_state_.mismatch_count <
                    fast_path.max_mismatches_before_disable) {
                    runtime_state_.disabled = false;
                }
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
