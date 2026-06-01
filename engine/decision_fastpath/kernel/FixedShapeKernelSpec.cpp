#include "engine/decision_fastpath/kernel/FixedShapeKernelSpec.h"

#include <algorithm>
#include <string>

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

void mix_i64(std::uint64_t* hash, std::int64_t value) noexcept {
    mix_u64(hash, static_cast<std::uint64_t>(value));
}

void mix_u32(std::uint64_t* hash, std::uint32_t value) noexcept {
    mix_u64(hash, value);
}

void mix_u8(std::uint64_t* hash, std::uint8_t value) noexcept {
    *hash ^= value;
    *hash *= kFnvPrime;
}

[[nodiscard]] std::uint32_t market_index_for(
    std::unordered_map<std::string, std::uint32_t>* market_indices,
    const std::string* market_id
) {
    if (market_id == nullptr || market_id->empty()) {
        return 0;
    }
    auto [it, inserted] = market_indices->emplace(
        *market_id,
        static_cast<std::uint32_t>(market_indices->size())
    );
    return it->second;
}

[[nodiscard]] FixedShapeKernelSpec spec_from_plan(
    const trading_engine::signal::BundleRuntimePlan& plan,
    std::unordered_map<std::string, std::uint32_t>* market_indices
) {
    FixedShapeKernelSpec spec;
    spec.artifact_hash = plan.oracle_artifact_hash;
    spec.bundle_hash = plan.bundle_hash;
    spec.constraint_hash = plan.constraint_hash;
    spec.bundle_id = plan.bundle_id;
    spec.leg_count = static_cast<std::uint8_t>(
        std::min<std::uint16_t>(plan.leg_count, kMaxFixedShapeKernelLegs)
    );
    spec.guaranteed_payout_tick = plan.guaranteed_payout_tick;
    spec.min_unit_edge_tick = plan.min_unit_edge_tick;
    spec.min_total_edge_tick = plan.min_total_edge_tick;
    spec.min_edge_bps = plan.min_edge_bps;
    spec.min_bundle_qty = 1;

    for (std::uint8_t i = 0; i < spec.leg_count; ++i) {
        spec.asset_indices[i] = plan.asset_indices[i];
        spec.market_indices[i] =
            market_index_for(market_indices, plan.market_ids[i]);
        spec.sides[i] = plan.sides[i];
        spec.ratio_qty_lots[i] = plan.ratio_qty_lots[i];
    }

    spec.kernel_spec_hash = hash_fixed_shape_kernel_spec(spec);
    return spec;
}

void rebuild_indexes(
    std::vector<FixedShapeKernelSpec>* specs,
    std::unordered_map<std::uint64_t, std::size_t>* bundle_index,
    std::unordered_map<std::uint32_t, std::vector<FixedShapeKernelSpec>>*
        asset_to_specs
) {
    std::sort(
        specs->begin(),
        specs->end(),
        [](const FixedShapeKernelSpec& lhs, const FixedShapeKernelSpec& rhs) {
            return lhs.bundle_id < rhs.bundle_id;
        }
    );

    bundle_index->clear();
    asset_to_specs->clear();

    for (std::size_t i = 0; i < specs->size(); ++i) {
        const auto& spec = (*specs)[i];
        const auto [_, inserted] = bundle_index->emplace(spec.bundle_id, i);
        if (!inserted) {
            specs->clear();
            bundle_index->clear();
            asset_to_specs->clear();
            return;
        }

        for (std::uint8_t leg_index = 0; leg_index < spec.leg_count;
             ++leg_index) {
            auto& bucket = (*asset_to_specs)[spec.asset_indices[leg_index]];
            const auto exists = std::any_of(
                bucket.begin(),
                bucket.end(),
                [&spec](const FixedShapeKernelSpec& existing) {
                    return existing.bundle_id == spec.bundle_id;
                }
            );
            if (!exists) {
                bucket.push_back(spec);
            }
        }
    }

    for (auto& [_, bucket] : *asset_to_specs) {
        std::sort(
            bucket.begin(),
            bucket.end(),
            [](const FixedShapeKernelSpec& lhs,
               const FixedShapeKernelSpec& rhs) {
                return lhs.bundle_id < rhs.bundle_id;
            }
        );
    }
}

}  // namespace

std::uint64_t hash_fixed_shape_kernel_spec(
    const FixedShapeKernelSpec& spec
) noexcept {
    auto hash = kFnvOffset;
    mix_u64(&hash, spec.artifact_hash);
    mix_u64(&hash, spec.bundle_hash);
    mix_u64(&hash, spec.constraint_hash);
    mix_u64(&hash, spec.bundle_id);
    mix_u8(&hash, spec.leg_count);

    for (std::uint8_t i = 0; i < spec.leg_count; ++i) {
        mix_u32(&hash, spec.asset_indices[i]);
        mix_u32(&hash, spec.market_indices[i]);
        mix_u8(&hash, static_cast<std::uint8_t>(spec.sides[i]));
        mix_i64(&hash, spec.ratio_qty_lots[i]);
    }

    mix_i64(&hash, spec.guaranteed_payout_tick);
    mix_i64(&hash, spec.min_unit_edge_tick);
    mix_i64(&hash, spec.min_total_edge_tick);
    mix_i64(&hash, spec.min_edge_bps);
    mix_i64(&hash, spec.min_bundle_qty);
    return hash;
}

void FixedShapeKernelRegistry::build_from_oracle_artifact(
    const OracleArtifact& artifact
) {
    build_from_runtime_plans(artifact.active_runtime_plans());
}

void FixedShapeKernelRegistry::build_from_runtime_plans(
    std::span<const trading_engine::signal::BundleRuntimePlan> plans
) {
    specs_.clear();
    specs_.reserve(plans.size());

    std::unordered_map<std::string, std::uint32_t> market_indices;
    for (const auto& plan : plans) {
        specs_.push_back(spec_from_plan(plan, &market_indices));
    }

    rebuild_indexes(&specs_, &bundle_index_, &asset_to_specs_);
}

const FixedShapeKernelSpec* FixedShapeKernelRegistry::find(
    std::uint64_t bundle_id
) const {
    const auto it = bundle_index_.find(bundle_id);
    if (it == bundle_index_.end()) {
        return nullptr;
    }
    return &specs_[it->second];
}

std::span<const FixedShapeKernelSpec> FixedShapeKernelRegistry::specs_for_asset(
    std::uint32_t asset_index
) const {
    const auto it = asset_to_specs_.find(asset_index);
    if (it == asset_to_specs_.end()) {
        return {};
    }
    return it->second;
}

std::span<const FixedShapeKernelSpec> FixedShapeKernelRegistry::specs()
    const noexcept {
    return specs_;
}

}  // namespace trading_engine::decision_fastpath
