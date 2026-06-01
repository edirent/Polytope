#pragma once

#include "engine/signal/reader/OracleArtifactReader.h"
#include "engine/signal/scan/BundleRuntimePlan.h"
#include "oracle/public/CandidateBundle.h"

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace trading_engine::decision_fastpath {

using Side = trading_engine::oracle::Side;
using OracleArtifact = trading_engine::signal::OracleArtifactReader;

inline constexpr std::uint8_t kMaxFixedShapeKernelLegs = 16;

struct FixedShapeKernelSpec {
    std::uint64_t kernel_spec_hash = 0;

    std::uint64_t artifact_hash = 0;
    std::uint64_t bundle_hash = 0;
    std::uint64_t constraint_hash = 0;

    std::uint64_t bundle_id = 0;

    std::uint8_t leg_count = 0;

    std::array<std::uint32_t, kMaxFixedShapeKernelLegs> asset_indices{};
    std::array<std::uint32_t, kMaxFixedShapeKernelLegs> market_indices{};

    std::array<Side, kMaxFixedShapeKernelLegs> sides{};

    std::array<std::int64_t, kMaxFixedShapeKernelLegs> ratio_qty_lots{};

    std::int64_t guaranteed_payout_tick = 0;

    std::int64_t min_unit_edge_tick = 0;
    std::int64_t min_total_edge_tick = 0;
    std::int64_t min_edge_bps = 0;
    std::int64_t min_bundle_qty = 0;
};

[[nodiscard]] std::uint64_t hash_fixed_shape_kernel_spec(
    const FixedShapeKernelSpec& spec
) noexcept;

class FixedShapeKernelRegistry {
public:
    void build_from_oracle_artifact(const OracleArtifact& artifact);

    void build_from_runtime_plans(
        std::span<const trading_engine::signal::BundleRuntimePlan> plans
    );

    [[nodiscard]] const FixedShapeKernelSpec* find(
        std::uint64_t bundle_id
    ) const;

    [[nodiscard]] std::span<const FixedShapeKernelSpec> specs_for_asset(
        std::uint32_t asset_index
    ) const;

    [[nodiscard]] std::span<const FixedShapeKernelSpec> specs() const noexcept;

private:
    std::vector<FixedShapeKernelSpec> specs_;
    std::unordered_map<std::uint64_t, std::size_t> bundle_index_;
    std::unordered_map<std::uint32_t, std::vector<FixedShapeKernelSpec>>
        asset_to_specs_;
};

}  // namespace trading_engine::decision_fastpath
