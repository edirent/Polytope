#pragma once

#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/signal/scan/BundleRuntimePlan.h"
#include "engine/state/view/MarketDepthView.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace trading_engine::decision_fastpath {

enum class FastPathRejectReason : std::uint8_t {
    None,
    MissingBundlePlan,
    MissingPolicy,
    EmptyBundle,
    TooManyLegs,
    SellLeg,
    UnsupportedExecutableSide,
    DynamicRuntimeOracleRequired,
    MissingDepthView,
    BadDepthView,
    ArtifactHashMismatch,
    PolicyHashMismatch,
    UniverseVersionMismatch,
    MissingSettlementMaskDependency,
    PolicyIncompatible,
    KernelSpecHashMismatch,
    RuntimeDisabled,
    Count
};

struct FastPathEligibility {
    bool eligible = false;
    FastPathRejectReason reason = FastPathRejectReason::None;
};

struct FastPathGateInput {
    const trading_engine::signal::BundleRuntimePlan* plan = nullptr;

    std::span<const trading_engine::state::MarketDepthView> depth_views{};

    const trading_engine::risk::RiskPolicySnapshot* policy = nullptr;

    std::uint64_t expected_oracle_artifact_hash = 0;
    std::uint64_t expected_policy_hash = 0;
    std::uint64_t expected_universe_version = 0;
    std::uint64_t current_universe_version = 0;

    bool requires_dynamic_runtime_oracle_check = false;
    bool settlement_masks_available = true;

    std::uint16_t max_supported_legs = 16;
};

struct FastPathGateStats {
    std::uint64_t evaluated = 0;
    std::uint64_t eligible = 0;
    std::uint64_t fallback = 0;
    std::array<
        std::uint64_t,
        static_cast<std::size_t>(FastPathRejectReason::Count)
    > by_reason{};
};

class FastPathGate {
public:
    [[nodiscard]] FastPathEligibility evaluate(
        const FastPathGateInput& input
    ) noexcept;

    [[nodiscard]] const FastPathGateStats& stats() const noexcept;
    void reset_stats() noexcept;

private:
    FastPathGateStats stats_;
};

[[nodiscard]] const char* fast_path_reject_reason_to_string(
    FastPathRejectReason reason
) noexcept;

}  // namespace trading_engine::decision_fastpath
