#include "engine/decision_fastpath/core/FastPathGate.h"

#include "engine/signal/pricing/SideResolver.h"
#include "oracle/public/CandidateBundle.h"

#include <algorithm>

namespace trading_engine::decision_fastpath {

namespace {

[[nodiscard]] bool has_settlement_mask_dependency(
    const trading_engine::signal::BundleRuntimePlan& plan
) noexcept {
    if (plan.bundle == nullptr) {
        return false;
    }
    return plan.bundle->required_true_mask != 0 ||
           plan.bundle->required_false_mask != 0 ||
           plan.bundle->invalid_mask != 0;
}

[[nodiscard]] bool policy_is_fast_path_compatible(
    const trading_engine::risk::RiskPolicySnapshot& policy
) noexcept {
    return policy.risk_enabled && !policy.kill_switch_enabled;
}

[[nodiscard]] const trading_engine::state::MarketDepthView* find_depth_view(
    std::span<const trading_engine::state::MarketDepthView> depth_views,
    std::uint32_t asset_index
) noexcept {
    for (const auto& view : depth_views) {
        if (view.asset_index == asset_index) {
            return &view;
        }
    }
    return nullptr;
}

[[nodiscard]] FastPathRejectReason first_reject_reason(
    const FastPathGateInput& input
) noexcept {
    const auto* plan = input.plan;
    if (plan == nullptr) {
        return FastPathRejectReason::MissingBundlePlan;
    }
    if (input.policy == nullptr) {
        return FastPathRejectReason::MissingPolicy;
    }
    if (plan->leg_count == 0) {
        return FastPathRejectReason::EmptyBundle;
    }
    const auto max_supported_legs =
        input.max_supported_legs == 0 ? 16 : input.max_supported_legs;
    if (plan->leg_count > max_supported_legs ||
        plan->leg_count > trading_engine::signal::kMaxIntentLegs) {
        return FastPathRejectReason::TooManyLegs;
    }
    if (input.requires_dynamic_runtime_oracle_check) {
        return FastPathRejectReason::DynamicRuntimeOracleRequired;
    }
    if (plan->oracle_artifact_hash == 0 ||
        input.expected_oracle_artifact_hash == 0 ||
        plan->oracle_artifact_hash != input.expected_oracle_artifact_hash) {
        return FastPathRejectReason::ArtifactHashMismatch;
    }
    if (input.policy->policy_hash == 0 || input.expected_policy_hash == 0 ||
        input.policy->policy_hash != input.expected_policy_hash) {
        return FastPathRejectReason::PolicyHashMismatch;
    }
    if (input.current_universe_version == 0 ||
        input.expected_universe_version == 0 ||
        input.current_universe_version != input.expected_universe_version) {
        return FastPathRejectReason::UniverseVersionMismatch;
    }
    if (!input.settlement_masks_available &&
        has_settlement_mask_dependency(*plan)) {
        return FastPathRejectReason::MissingSettlementMaskDependency;
    }
    if (!policy_is_fast_path_compatible(*input.policy)) {
        return FastPathRejectReason::PolicyIncompatible;
    }

    for (std::uint16_t i = 0; i < plan->leg_count; ++i) {
        if (plan->sides[i] != trading_engine::oracle::Side::Buy) {
            return FastPathRejectReason::SellLeg;
        }
        if (plan->executable_sides[i] !=
            trading_engine::signal::ExecutableBookSide::Asks) {
            return FastPathRejectReason::UnsupportedExecutableSide;
        }

        const auto* depth_view =
            find_depth_view(input.depth_views, plan->asset_indices[i]);
        if (depth_view == nullptr) {
            return FastPathRejectReason::MissingDepthView;
        }
        if (!depth_view->usable_for_depth || depth_view->recovering ||
            depth_view->crossed || depth_view->closed ||
            depth_view->resolved || depth_view->ask_count == 0) {
            return FastPathRejectReason::BadDepthView;
        }
    }

    return FastPathRejectReason::None;
}

void record(
    FastPathGateStats* stats,
    const FastPathEligibility& eligibility
) noexcept {
    ++stats->evaluated;
    if (eligibility.eligible) {
        ++stats->eligible;
        return;
    }
    ++stats->fallback;
    const auto index = static_cast<std::size_t>(eligibility.reason);
    if (index < stats->by_reason.size()) {
        ++stats->by_reason[index];
    }
}

}  // namespace

FastPathEligibility FastPathGate::evaluate(
    const FastPathGateInput& input
) noexcept {
    const auto reason = first_reject_reason(input);
    FastPathEligibility result;
    result.eligible = reason == FastPathRejectReason::None;
    result.reason = reason;
    record(&stats_, result);
    return result;
}

const FastPathGateStats& FastPathGate::stats() const noexcept {
    return stats_;
}

void FastPathGate::reset_stats() noexcept {
    stats_ = FastPathGateStats{};
}

const char* fast_path_reject_reason_to_string(
    FastPathRejectReason reason
) noexcept {
    switch (reason) {
        case FastPathRejectReason::None:
            return "None";
        case FastPathRejectReason::MissingBundlePlan:
            return "MissingBundlePlan";
        case FastPathRejectReason::MissingPolicy:
            return "MissingPolicy";
        case FastPathRejectReason::EmptyBundle:
            return "EmptyBundle";
        case FastPathRejectReason::TooManyLegs:
            return "TooManyLegs";
        case FastPathRejectReason::SellLeg:
            return "SellLeg";
        case FastPathRejectReason::UnsupportedExecutableSide:
            return "UnsupportedExecutableSide";
        case FastPathRejectReason::DynamicRuntimeOracleRequired:
            return "DynamicRuntimeOracleRequired";
        case FastPathRejectReason::MissingDepthView:
            return "MissingDepthView";
        case FastPathRejectReason::BadDepthView:
            return "BadDepthView";
        case FastPathRejectReason::ArtifactHashMismatch:
            return "ArtifactHashMismatch";
        case FastPathRejectReason::PolicyHashMismatch:
            return "PolicyHashMismatch";
        case FastPathRejectReason::UniverseVersionMismatch:
            return "UniverseVersionMismatch";
        case FastPathRejectReason::MissingSettlementMaskDependency:
            return "MissingSettlementMaskDependency";
        case FastPathRejectReason::PolicyIncompatible:
            return "PolicyIncompatible";
        case FastPathRejectReason::KernelSpecHashMismatch:
            return "KernelSpecHashMismatch";
        case FastPathRejectReason::RuntimeDisabled:
            return "RuntimeDisabled";
        case FastPathRejectReason::Count:
            break;
    }
    return "Unknown";
}

}  // namespace trading_engine::decision_fastpath
