#pragma once

#include "engine/decision_fastpath/core/DifferentialVerifier.h"
#include "engine/decision_fastpath/core/EventLocalDecisionPipeline.h"
#include "engine/signal/reader/OracleArtifactReader.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace decision_fastpath_test {

namespace fast = trading_engine::decision_fastpath;
namespace risk = trading_engine::risk;
namespace signal = trading_engine::signal;
namespace state = trading_engine::state;

inline state::MarketDepthView make_depth_view(
    std::uint32_t asset_index,
    std::int64_t ask_price_tick = 400'000,
    double size = 10.0
) {
    state::MarketDepthView view;
    view.asset_index = asset_index;
    view.book_version = 10 + asset_index;
    view.snapshot_version_hash = 1'000 + asset_index;
    view.last_ws_recv_ns = 1'000;
    view.usable_for_depth = true;
    view.ask_count = 2;
    view.asks[0] = state::PriceLevel{
        .price_tick = ask_price_tick,
        .price = static_cast<double>(ask_price_tick) / 1'000'000.0,
        .size = size
    };
    view.asks[1] = state::PriceLevel{
        .price_tick = ask_price_tick + 10'000,
        .price = static_cast<double>(ask_price_tick + 10'000) / 1'000'000.0,
        .size = size
    };
    state::build_depth_prefix(
        view.bids,
        view.bid_count,
        view.asks,
        view.ask_count,
        &view.prefix
    );
    return view;
}

inline risk::RiskPolicySnapshot policy() {
    risk::RiskPolicySnapshot out;
    out.max_book_age_ns = 1'000'000'000;
    out.min_depth_margin_ratio = 1.0;
    out.min_depth_margin_bps = 10'000;
    out.max_approvals_per_second = 1'000'000;
    return risk::with_computed_policy_hash(out);
}

inline std::vector<state::MarketDepthView> depths_for_specs(
    std::span<const fast::FixedShapeKernelSpec> specs
) {
    std::unordered_map<std::uint32_t, state::MarketDepthView> by_asset;
    for (const auto& spec : specs) {
        for (std::uint8_t i = 0; i < spec.leg_count; ++i) {
            by_asset.emplace(
                spec.asset_indices[i],
                make_depth_view(spec.asset_indices[i])
            );
        }
    }

    std::vector<state::MarketDepthView> depths;
    depths.reserve(by_asset.size());
    for (const auto& [_, depth] : by_asset) {
        depths.push_back(depth);
    }
    std::sort(
        depths.begin(),
        depths.end(),
        [](const state::MarketDepthView& lhs,
           const state::MarketDepthView& rhs) {
            return lhs.asset_index < rhs.asset_index;
        }
    );
    return depths;
}

inline fast::DecisionPathSnapshot best_generic_for_dirty_asset(
    std::span<const fast::FixedShapeKernelSpec> specs,
    std::span<const state::MarketDepthView> depths,
    const risk::RiskPolicySnapshot& policy,
    const risk::RiskLedgerSnapshot& ledger,
    std::uint64_t now_ns
) {
    fast::DecisionPathSnapshot best;
    bool have_best = false;

    for (const auto& spec : specs) {
        const auto candidate = fast::reference_generic_decision(
            spec,
            depths,
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

inline signal::OracleArtifactReader load_artifact(
    const std::filesystem::path& path
) {
    signal::OracleArtifactReader reader;
    const auto result = reader.load(path);
    if (!result.ok) {
        throw std::runtime_error("failed to load artifact: " + result.error);
    }
    return reader;
}

inline fast::FixedShapeKernelRegistry registry_from_artifact(
    const std::filesystem::path& path
) {
    auto reader = load_artifact(path);
    fast::FixedShapeKernelRegistry registry;
    registry.build_from_oracle_artifact(reader);
    return registry;
}

inline void expect_shadow_matches_generic_for_all_dirty_assets(
    const fast::FixedShapeKernelRegistry& registry,
    const risk::RiskPolicySnapshot& policy,
    std::span<const state::MarketDepthView> depths
) {
    const risk::RiskLedgerSnapshot ledger;
    fast::EventLocalDecisionPipelineConfig config;
    config.expected_policy_hash = policy.policy_hash;
    config.mode = fast::EventLocalPipelineMode::ShadowCompare;
    config.fast_path.mode = fast::FastPathMode::ShadowCompare;
    config.fast_path.enable_fixed_buy_kernel = true;
    config.fast_path.sample_verify_rate = 1.0;
    const auto all_specs = registry.specs();
    if (!all_specs.empty()) {
        config.expected_artifact_hash = all_specs.front().artifact_hash;
        config.expected_constraint_hash = all_specs.front().constraint_hash;
    }

    fast::EventLocalDecisionPipeline pipeline{&registry, config};

    std::vector<std::uint32_t> dirty_assets;
    for (const auto& spec : all_specs) {
        for (std::uint8_t i = 0; i < spec.leg_count; ++i) {
            dirty_assets.push_back(spec.asset_indices[i]);
        }
    }
    std::sort(dirty_assets.begin(), dirty_assets.end());
    dirty_assets.erase(
        std::unique(dirty_assets.begin(), dirty_assets.end()),
        dirty_assets.end()
    );

    for (const auto asset_index : dirty_assets) {
        const auto fast_result = pipeline.process({
            .now_ns = 2'000,
            .dirty_asset_index = asset_index,
            .depth_views = depths.data(),
            .depth_view_count = static_cast<std::uint16_t>(depths.size()),
            .policy = &policy,
            .ledger = &ledger,
            .current_true_mask = 0,
            .current_false_mask = 0
        });
        if (fast_result.fallback_required) {
            continue;
        }

        const auto generic = best_generic_for_dirty_asset(
            registry.specs_for_asset(asset_index),
            depths,
            policy,
            ledger,
            2'000
        );
        const auto diff = fast::compare_decision_snapshots(
            fast::snapshot_from_fast_result(fast_result),
            generic
        );
        if (!diff.match) {
            throw std::runtime_error(
                "shadow mismatch asset=" + std::to_string(asset_index) +
                " field=" + diff.first_mismatch +
                " fast_hash=" + std::to_string(diff.fast_hash) +
                " generic_hash=" + std::to_string(diff.generic_hash)
            );
        }
    }
}

}  // namespace decision_fastpath_test
