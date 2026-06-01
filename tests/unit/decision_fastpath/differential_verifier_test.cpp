#include "engine/decision_fastpath/core/DifferentialVerifier.h"
#include "engine/decision_fastpath/core/EventLocalDecisionPipeline.h"
#include "engine/decision_fastpath/core/FastPathGate.h"
#include "engine/signal/scan/BundleRuntimePlan.h"
#include "oracle/public/CandidateBundle.h"

#include <array>
#include <exception>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

namespace fast = trading_engine::decision_fastpath;
namespace oracle = trading_engine::oracle;
namespace risk = trading_engine::risk;
namespace signal = trading_engine::signal;
namespace state = trading_engine::state;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

void expect_false(bool value, const std::string& field) {
    if (value) {
        fail("expected false: " + field);
    }
}

template <typename Actual, typename Expected>
void expect_equal(
    const Actual& actual,
    const Expected& expected,
    const std::string& field
) {
    if (!(actual == expected)) {
        fail("mismatch: " + field);
    }
}

state::MarketDepthView depth_view(
    std::uint32_t asset_index,
    std::initializer_list<std::pair<std::int64_t, double>> asks
) {
    state::MarketDepthView view;
    view.asset_index = asset_index;
    view.book_version = 10 + asset_index;
    view.snapshot_version_hash = 1'000 + asset_index;
    view.last_ws_recv_ns = 1'000;
    view.usable_for_depth = true;
    view.ask_count = static_cast<std::uint16_t>(asks.size());

    std::uint16_t i = 0;
    for (const auto& [price, size] : asks) {
        view.asks[i++] = state::PriceLevel{
            .price_tick = price,
            .price = static_cast<double>(price) / 1'000'000.0,
            .size = size
        };
    }
    state::build_depth_prefix(
        view.bids,
        view.bid_count,
        view.asks,
        view.ask_count,
        &view.prefix
    );
    return view;
}

fast::FixedShapeKernelSpec base_spec() {
    fast::FixedShapeKernelSpec spec;
    spec.artifact_hash = 9'001;
    spec.constraint_hash = 8'001;
    spec.bundle_hash = 7'001;
    spec.bundle_id = 17;
    spec.leg_count = 2;
    spec.asset_indices[0] = 100;
    spec.asset_indices[1] = 101;
    spec.market_indices[0] = 200;
    spec.market_indices[1] = 201;
    spec.sides[0] = oracle::Side::Buy;
    spec.sides[1] = oracle::Side::Buy;
    spec.ratio_qty_lots[0] = 1;
    spec.ratio_qty_lots[1] = 1;
    spec.guaranteed_payout_tick = 1'000'000;
    spec.min_unit_edge_tick = 1;
    spec.min_total_edge_tick = 1;
    spec.min_edge_bps = 0;
    spec.min_bundle_qty = 1;
    spec.kernel_spec_hash = fast::hash_fixed_shape_kernel_spec(spec);
    return spec;
}

risk::RiskPolicySnapshot default_policy() {
    auto policy = risk::RiskPolicySnapshot{};
    policy.max_book_age_ns = 1'000'000'000;
    policy.min_depth_margin_ratio = 1.0;
    policy.min_depth_margin_bps = 10'000;
    return risk::with_computed_policy_hash(policy);
}

std::array<state::MarketDepthView, 2> base_depths(
    std::int64_t price = 400'000,
    double size = 10.0
) {
    return {
        depth_view(100, {{price, size}}),
        depth_view(101, {{price, size}}),
    };
}

fast::FastKernelResult fast_run(
    const fast::FixedShapeKernelSpec& spec,
    const state::MarketDepthView* depths,
    std::uint16_t depth_count,
    const risk::RiskPolicySnapshot& policy,
    const risk::RiskLedgerSnapshot& ledger
) {
    fast::FixedBuyBundleKernelScalar kernel;
    fast::FastPathScratch scratch;
    return kernel.run(
        spec,
        depths,
        depth_count,
        policy,
        ledger,
        2'000,
        &scratch
    );
}

void expect_fast_matches_generic(
    const fast::FixedShapeKernelSpec& spec,
    const state::MarketDepthView* depths,
    std::uint16_t depth_count,
    const risk::RiskPolicySnapshot& policy,
    const risk::RiskLedgerSnapshot& ledger
) {
    oracle::CandidateBundle bundle;
    bundle.bundle_id = spec.bundle_id;
    bundle.guaranteed_payout_tick = spec.guaranteed_payout_tick;
    bundle.min_edge_tick = spec.min_unit_edge_tick;
    bundle.leg_count = spec.leg_count;
    signal::BundleRuntimePlan plan;
    plan.bundle = &bundle;
    plan.bundle_id = spec.bundle_id;
    plan.bundle_hash = spec.bundle_hash;
    plan.oracle_artifact_hash = spec.artifact_hash;
    plan.constraint_hash = spec.constraint_hash;
    plan.leg_count = spec.leg_count;
    plan.unique_asset_count = spec.leg_count;
    plan.guaranteed_payout_tick = spec.guaranteed_payout_tick;
    plan.min_unit_edge_tick = spec.min_unit_edge_tick;
    plan.min_total_edge_tick = spec.min_total_edge_tick;
    plan.min_edge_bps = spec.min_edge_bps;
    for (std::uint16_t i = 0; i < spec.leg_count; ++i) {
        bundle.legs[i] = oracle::BundleLeg{
            .market_id = "market-" + std::to_string(spec.market_indices[i]),
            .asset_id = "asset-" + std::to_string(spec.asset_indices[i]),
            .side = spec.sides[i],
            .quantity_lots = spec.ratio_qty_lots[i],
            .max_price_tick = 1'000'000
        };
        plan.market_ids[i] = &bundle.legs[i].market_id;
        plan.asset_ids[i] = &bundle.legs[i].asset_id;
        plan.asset_indices[i] = spec.asset_indices[i];
        plan.unique_asset_ids[i] = &bundle.legs[i].asset_id;
        plan.unique_asset_indices[i] = spec.asset_indices[i];
        plan.sides[i] = spec.sides[i];
        plan.ratio_qty_lots[i] = spec.ratio_qty_lots[i];
        plan.max_price_ticks[i] = 1'000'000;
    }
    std::array<signal::BundleRuntimePlan, 1> plans{plan};
    fast::FixedShapeKernelRegistry registry;
    registry.build_from_runtime_plans(plans);
    fast::EventLocalDecisionPipelineConfig config;
    config.expected_artifact_hash = spec.artifact_hash;
    config.expected_constraint_hash = spec.constraint_hash;
    config.expected_policy_hash = policy.policy_hash;
    config.mode = fast::EventLocalPipelineMode::ShadowCompare;
    config.fast_path.mode = fast::FastPathMode::ShadowCompare;
    config.fast_path.enable_fixed_buy_kernel = true;
    fast::EventLocalDecisionPipeline pipeline{&registry, config};
    const auto result = pipeline.process(fast::EventLocalInput{
        .now_ns = 2'000,
        .dirty_asset_index = spec.asset_indices[0],
        .depth_views = depths,
        .depth_view_count = depth_count,
        .policy = &policy,
        .ledger = &ledger
    });
    if (result.generic_comparison_performed && result.mismatch) {
        fail(
            "fast/generic mismatch at " + result.mismatch_reason +
            " fast_hash=" + std::to_string(result.fast_combined_hash) +
            " generic_hash=" + std::to_string(result.generic_combined_hash) +
            " produced_plan=" + std::to_string(result.produced_plan ? 1 : 0) +
            " status=" +
            std::to_string(static_cast<int>(result.intent.status)) +
            " risk=" +
            std::to_string(static_cast<int>(result.decision.reject_reason)) +
            " qty=" + std::to_string(result.intent.bundle_qty) +
            " cost=" + std::to_string(result.intent.estimated_cost_tick) +
            " edge=" + std::to_string(result.intent.total_edge_tick)
        );
    }
    if (result.generic_comparison_performed) {
        expect_equal(
            result.fast_combined_hash,
            result.generic_combined_hash,
            "combined hash"
        );
    }
    expect_true(result.output_hash != 0, "output hash");
}

signal::BundleRuntimePlan runtime_plan_for(
    const oracle::CandidateBundle& bundle
) {
    signal::BundleRuntimePlan plan;
    plan.bundle = &bundle;
    plan.bundle_id = bundle.bundle_id;
    plan.bundle_hash = 7'001;
    plan.oracle_artifact_hash = 9'001;
    plan.constraint_hash = 8'001;
    plan.leg_count = bundle.leg_count;
    plan.unique_asset_count = bundle.leg_count;
    plan.guaranteed_payout_tick = bundle.guaranteed_payout_tick;
    plan.min_unit_edge_tick = bundle.min_edge_tick;
    plan.min_total_edge_tick = bundle.min_edge_tick;
    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        plan.market_ids[i] = &bundle.legs[i].market_id;
        plan.asset_ids[i] = &bundle.legs[i].asset_id;
        plan.asset_indices[i] = 100 + i;
        plan.unique_asset_ids[i] = &bundle.legs[i].asset_id;
        plan.unique_asset_indices[i] = 100 + i;
        plan.sides[i] = bundle.legs[i].side;
        plan.executable_sides[i] =
            bundle.legs[i].side == oracle::Side::Buy
                ? signal::ExecutableBookSide::Asks
                : signal::ExecutableBookSide::Unsupported;
        plan.ratio_qty_lots[i] = bundle.legs[i].quantity_lots;
        plan.max_price_ticks[i] = bundle.legs[i].max_price_tick;
    }
    return plan;
}

oracle::CandidateBundle candidate_bundle() {
    oracle::CandidateBundle bundle;
    bundle.bundle_id = 17;
    bundle.guaranteed_payout_tick = 1'000'000;
    bundle.min_edge_tick = 1;
    bundle.leg_count = 2;
    bundle.legs[0] = oracle::BundleLeg{
        .market_id = "market-a",
        .asset_id = "asset-a",
        .side = oracle::Side::Buy,
        .quantity_lots = 1,
        .max_price_tick = 1'000'000
    };
    bundle.legs[1] = oracle::BundleLeg{
        .market_id = "market-b",
        .asset_id = "asset-b",
        .side = oracle::Side::Buy,
        .quantity_lots = 1,
        .max_price_tick = 1'000'000
    };
    return bundle;
}

void FastKernel_MatchesGenericPositiveOpportunity() {
    const auto spec = base_spec();
    const auto policy = default_policy();
    const risk::RiskLedgerSnapshot ledger;
    const auto depths = base_depths();

    expect_fast_matches_generic(
        spec,
        depths.data(),
        static_cast<std::uint16_t>(depths.size()),
        policy,
        ledger
    );
}

void FastKernel_OutputHashesMatchGeneric() {
    const auto spec = base_spec();
    const auto policy = default_policy();
    const risk::RiskLedgerSnapshot ledger;
    const auto depths = base_depths();
    expect_fast_matches_generic(
        spec,
        depths.data(),
        static_cast<std::uint16_t>(depths.size()),
        policy,
        ledger
    );
}

void FastKernel_MatchesGenericInsufficientDepth() {
    const auto spec = base_spec();
    const auto policy = default_policy();
    const risk::RiskLedgerSnapshot ledger;
    const auto depths = base_depths(400'000, 0.0);

    expect_fast_matches_generic(
        spec,
        depths.data(),
        static_cast<std::uint16_t>(depths.size()),
        policy,
        ledger
    );
}

void FastKernel_MatchesGenericLowEdge() {
    const auto spec = base_spec();
    const auto policy = default_policy();
    const risk::RiskLedgerSnapshot ledger;
    const auto depths = base_depths(600'000, 10.0);

    expect_fast_matches_generic(
        spec,
        depths.data(),
        static_cast<std::uint16_t>(depths.size()),
        policy,
        ledger
    );
}

void FastKernel_MatchesGenericRiskReject() {
    const auto spec = base_spec();
    auto policy = default_policy();
    policy.max_total_exposure_tick = 10'000'000;
    policy = risk::with_computed_policy_hash(policy);
    risk::RiskLedgerSnapshot ledger;
    ledger.total_reserved_exposure_tick = 3'000'001;
    const auto depths = base_depths();

    expect_fast_matches_generic(
        spec,
        depths.data(),
        static_cast<std::uint16_t>(depths.size()),
        policy,
        ledger
    );
}

void FastKernel_FallbackOnSellLeg() {
    auto spec = base_spec();
    spec.sides[0] = oracle::Side::Sell;
    const auto policy = default_policy();
    const risk::RiskLedgerSnapshot ledger;
    const auto depths = base_depths();

    expect_fast_matches_generic(
        spec,
        depths.data(),
        static_cast<std::uint16_t>(depths.size()),
        policy,
        ledger
    );
}

void FastKernel_FallbackOnDynamicOracleBundle() {
    auto bundle = candidate_bundle();
    auto plan = runtime_plan_for(bundle);
    const auto policy = default_policy();
    const auto depths = base_depths();

    fast::FastPathGate gate;
    const auto eligibility = gate.evaluate({
        .plan = &plan,
        .depth_views = std::span<const state::MarketDepthView>{
            depths.data(),
            depths.size()
        },
        .policy = &policy,
        .expected_oracle_artifact_hash = plan.oracle_artifact_hash,
        .expected_policy_hash = policy.policy_hash,
        .expected_universe_version = 1,
        .current_universe_version = 1,
        .requires_dynamic_runtime_oracle_check = true,
        .settlement_masks_available = true,
        .max_supported_legs = 16
    });

    expect_false(eligibility.eligible, "eligible");
    expect_equal(
        eligibility.reason,
        fast::FastPathRejectReason::DynamicRuntimeOracleRequired,
        "reason"
    );
}

void FastKernel_NoReservationInShadowMode() {
    auto bundle = candidate_bundle();
    const auto plan = runtime_plan_for(bundle);
    std::array<signal::BundleRuntimePlan, 1> plans{plan};
    fast::FixedShapeKernelRegistry registry;
    registry.build_from_runtime_plans(plans);
    const auto policy = default_policy();
    const auto depths = base_depths();
    fast::EventLocalDecisionPipelineConfig config;
    config.expected_artifact_hash = plan.oracle_artifact_hash;
    config.expected_constraint_hash = plan.constraint_hash;
    config.expected_policy_hash = policy.policy_hash;
    config.mode = fast::EventLocalPipelineMode::ShadowCompare;
    config.fast_path.mode = fast::FastPathMode::ShadowCompare;
    config.fast_path.enable_fixed_buy_kernel = true;

    fast::EventLocalDecisionPipeline pipeline{&registry, config};
    const auto result = pipeline.process({
        .now_ns = 2'000,
        .dirty_asset_index = 100,
        .depth_views = depths.data(),
        .depth_view_count = static_cast<std::uint16_t>(depths.size()),
        .policy = &policy,
        .ledger = nullptr,
        .current_true_mask = 0,
        .current_false_mask = 0
    });

    expect_true(result.produced_plan, "produced plan");
    expect_false(result.publish_allowed, "publish");
    expect_false(result.reservation_allowed, "reserve");
    expect_false(result.approved.has_reservation(), "reservation");
}

void FastKernel_DeterministicOutputHash() {
    const auto spec = base_spec();
    const auto policy = default_policy();
    const risk::RiskLedgerSnapshot ledger;
    const auto depths = base_depths();

    const auto first = fast_run(
        spec,
        depths.data(),
        static_cast<std::uint16_t>(depths.size()),
        policy,
        ledger
    );
    const auto second = fast_run(
        spec,
        depths.data(),
        static_cast<std::uint16_t>(depths.size()),
        policy,
        ledger
    );

    expect_equal(first.output_hash, second.output_hash, "kernel hash");
    expect_equal(
        fast::snapshot_from_fast_kernel(first).semantic_hash,
        fast::snapshot_from_fast_kernel(second).semantic_hash,
        "semantic hash"
    );
    expect_equal(
        fast::snapshot_from_fast_kernel(first).combined_hash,
        fast::snapshot_from_fast_kernel(second).combined_hash,
        "combined hash"
    );
}

void FastKernel_RandomPropertyMatchesGeneric10k() {
    std::mt19937_64 rng{0x5eed1234ULL};

    for (std::uint32_t case_index = 0; case_index < 10'000; ++case_index) {
        fast::FixedShapeKernelSpec spec;
        spec.artifact_hash = 9'001;
        spec.constraint_hash = 8'001;
        spec.bundle_hash = 7'001 + case_index;
        spec.bundle_id = 100'000 + case_index;
        spec.leg_count = static_cast<std::uint8_t>(1 + (rng() % 4));
        spec.guaranteed_payout_tick =
            500'000 + static_cast<std::int64_t>(rng() % 1'500'000);
        spec.min_unit_edge_tick = 0;
        spec.min_total_edge_tick = 0;
        spec.min_edge_bps = 0;
        spec.min_bundle_qty = 1;

        std::array<state::MarketDepthView, 4> depths{};
        const auto shared_size = static_cast<double>(1 + (rng() % 20));
        const auto shared_price =
            50'000 + static_cast<std::int64_t>(rng() % 250'000);
        for (std::uint8_t i = 0; i < spec.leg_count; ++i) {
            spec.asset_indices[i] = 100 + i;
            spec.market_indices[i] = 200 + i;
            spec.sides[i] = oracle::Side::Buy;
            spec.ratio_qty_lots[i] = 1;

            state::MarketDepthView view;
            view.asset_index = spec.asset_indices[i];
            view.book_version = 10 + i;
            view.snapshot_version_hash = 1'000 + i;
            view.last_ws_recv_ns = 1'000;
            view.usable_for_depth = (rng() % 25) != 0;
            view.crossed = (rng() % 100) == 0;
            view.ask_count = 1;
            for (std::uint16_t level = 0; level < view.ask_count; ++level) {
                view.asks[level] = state::PriceLevel{
                    .price_tick = shared_price,
                    .price = static_cast<double>(shared_price) / 1'000'000.0,
                    .size = shared_size
                };
            }
            state::build_depth_prefix(
                view.bids,
                view.bid_count,
                view.asks,
                view.ask_count,
                &view.prefix
            );
            depths[i] = view;
        }
        spec.kernel_spec_hash = fast::hash_fixed_shape_kernel_spec(spec);

        auto policy = default_policy();
        policy = risk::with_computed_policy_hash(policy);

        risk::RiskLedgerSnapshot ledger;

        expect_fast_matches_generic(
            spec,
            depths.data(),
            spec.leg_count,
            policy,
            ledger
        );
    }
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"FastKernel_MatchesGenericPositiveOpportunity",
         &FastKernel_MatchesGenericPositiveOpportunity},
        {"FastKernel_OutputHashesMatchGeneric",
         &FastKernel_OutputHashesMatchGeneric},
        {"FastKernel_MatchesGenericInsufficientDepth",
         &FastKernel_MatchesGenericInsufficientDepth},
        {"FastKernel_MatchesGenericLowEdge",
         &FastKernel_MatchesGenericLowEdge},
        {"FastKernel_MatchesGenericRiskReject",
         &FastKernel_MatchesGenericRiskReject},
        {"FastKernel_FallbackOnSellLeg", &FastKernel_FallbackOnSellLeg},
        {"FastKernel_FallbackOnDynamicOracleBundle",
         &FastKernel_FallbackOnDynamicOracleBundle},
        {"FastKernel_NoReservationInShadowMode",
         &FastKernel_NoReservationInShadowMode},
        {"FastKernel_DeterministicOutputHash",
         &FastKernel_DeterministicOutputHash},
        {"FastKernel_RandomPropertyMatchesGeneric10k",
         &FastKernel_RandomPropertyMatchesGeneric10k},
    };
    return test_map;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <test-name>\n";
        return 2;
    }

    const auto it = tests().find(argv[1]);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << argv[1] << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << argv[1] << " failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
