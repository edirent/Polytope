#include "engine/decision_fastpath/core/EventLocalDecisionPipeline.h"

#include "engine/signal/scan/BundleRuntimePlan.h"
#include "oracle/public/CandidateBundle.h"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

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
    std::int64_t ask_price_tick,
    double size
) {
    state::MarketDepthView view;
    view.asset_index = asset_index;
    view.book_version = 10 + asset_index;
    view.snapshot_version_hash = 1'000 + asset_index;
    view.last_ws_recv_ns = 1'000;
    view.usable_for_depth = true;
    view.ask_count = 1;
    view.asks[0] = state::PriceLevel{
        .price_tick = ask_price_tick,
        .price = static_cast<double>(ask_price_tick) / 1'000'000.0,
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

struct Fixture {
    oracle::CandidateBundle bundle;
    signal::BundleRuntimePlan plan;
    std::array<signal::BundleRuntimePlan, 1> plans{};
    fast::FixedShapeKernelRegistry registry;
    risk::RiskPolicySnapshot policy;
    risk::RiskLedgerSnapshot ledger;
    std::array<state::MarketDepthView, 2> depth_views{};
};

void populate_runtime_plan(Fixture* out) {
    out->plan.bundle = &out->bundle;
    out->plan.bundle_id = out->bundle.bundle_id;
    out->plan.bundle_hash = 7'001;
    out->plan.oracle_artifact_hash = 9'001;
    out->plan.constraint_hash = 8'001;
    out->plan.leg_count = out->bundle.leg_count;
    out->plan.unique_asset_count = out->bundle.leg_count;
    out->plan.guaranteed_payout_tick = out->bundle.guaranteed_payout_tick;
    out->plan.min_unit_edge_tick = out->bundle.min_edge_tick;
    out->plan.min_total_edge_tick = out->bundle.min_edge_tick;
    out->plan.min_edge_bps = 0;

    for (std::uint16_t i = 0; i < out->bundle.leg_count; ++i) {
        const auto& leg = out->bundle.legs[i];
        out->plan.market_ids[i] = &leg.market_id;
        out->plan.asset_ids[i] = &leg.asset_id;
        out->plan.asset_indices[i] = 100 + i;
        out->plan.unique_asset_ids[i] = &leg.asset_id;
        out->plan.unique_asset_indices[i] = 100 + i;
        out->plan.sides[i] = leg.side;
        out->plan.ratio_qty_lots[i] = leg.quantity_lots;
        out->plan.max_price_ticks[i] = leg.max_price_tick;
    }
    out->plans[0] = out->plan;
    out->registry.build_from_runtime_plans(out->plans);
}

Fixture fixture(std::int64_t ask_price_tick = 400'000) {
    Fixture out;
    out.bundle.bundle_id = 17;
    out.bundle.guaranteed_payout_tick = 1'000'000;
    out.bundle.min_edge_tick = 1;
    out.bundle.leg_count = 2;
    out.bundle.legs[0] = oracle::BundleLeg{
        .market_id = "market-a",
        .asset_id = "asset-a",
        .side = oracle::Side::Buy,
        .quantity_lots = 1,
        .max_price_tick = 1'000'000
    };
    out.bundle.legs[1] = oracle::BundleLeg{
        .market_id = "market-b",
        .asset_id = "asset-b",
        .side = oracle::Side::Buy,
        .quantity_lots = 1,
        .max_price_tick = 1'000'000
    };

    out.policy = risk::RiskPolicySnapshot{};
    out.policy.min_depth_margin_ratio = 1.0;
    out.policy.min_depth_margin_bps = 10'000;
    out.policy = risk::with_computed_policy_hash(out.policy);
    out.depth_views[0] = depth_view(100, ask_price_tick, 10.0);
    out.depth_views[1] = depth_view(101, ask_price_tick, 10.0);
    populate_runtime_plan(&out);
    return out;
}

fast::EventLocalDecisionPipelineConfig config_for(
    const Fixture& data,
    fast::EventLocalPipelineMode mode =
        fast::EventLocalPipelineMode::ShadowCompare
) {
    fast::EventLocalDecisionPipelineConfig config;
    config.expected_artifact_hash = data.plan.oracle_artifact_hash;
    config.expected_constraint_hash = data.plan.constraint_hash;
    config.expected_policy_hash = data.policy.policy_hash;
    config.intent_ttl_ns = 10'000;
    config.mode = mode;
    config.fast_path.mode = mode;
    config.fast_path.enable_fixed_buy_kernel = true;
    return config;
}

fast::EventLocalInput input_for(Fixture& data, std::uint32_t dirty_asset = 100) {
    return fast::EventLocalInput{
        .now_ns = 2'000,
        .dirty_asset_index = dirty_asset,
        .depth_views = data.depth_views.data(),
        .depth_view_count = static_cast<std::uint16_t>(data.depth_views.size()),
        .policy = &data.policy,
        .ledger = &data.ledger,
        .current_true_mask = 0,
        .current_false_mask = 0
    };
}

void EventLocalDecisionPipeline_ProducesPlanForEligibleSpec() {
    auto data = fixture();
    auto config = config_for(
        data,
        fast::EventLocalPipelineMode::PaperAuthoritative
    );
    fast::EventLocalDecisionPipeline pipeline{
        &data.registry,
        config
    };

    const auto result = pipeline.process(input_for(data));

    expect_true(result.produced_plan, "produced plan");
    expect_false(result.fallback_required, "fallback");
    expect_equal(
        result.mode,
        fast::EventLocalPipelineMode::PaperAuthoritative,
        "mode"
    );
    expect_true(result.publish_allowed, "publish allowed");
    expect_true(result.reservation_allowed, "reservation allowed");
    expect_false(result.generic_verification_required, "generic verification");
    expect_true(result.authoritative, "authoritative");
    expect_equal(result.reject_reason, fast::FastPathRejectReason::None, "reason");
    expect_equal(
        result.intent.status,
        signal::IntentStatus::PaperOpportunity,
        "intent status"
    );
    expect_true(result.decision.approved(), "risk approved");
    expect_true(result.approved.has_reservation(), "reservation");
    expect_equal(result.plan.order_count, static_cast<std::uint16_t>(2), "orders");
    expect_equal(result.plan.bundle_id, 17ULL, "plan bundle");
    expect_equal(result.plan.orders[0].quantity_lots, 10LL, "order qty");
    expect_equal(result.intent.bundle_qty, 10LL, "bundle qty");
    expect_true(result.output_hash != 0, "output hash");
}

void EventLocalDecisionPipeline_ShadowCompareDoesNotPublishOrReserve() {
    auto data = fixture();
    fast::EventLocalDecisionPipeline pipeline{
        &data.registry,
        config_for(data)
    };

    const auto result = pipeline.process(input_for(data));

    expect_true(result.produced_plan, "produced plan");
    expect_false(result.fallback_required, "fallback");
    expect_equal(
        result.mode,
        fast::EventLocalPipelineMode::ShadowCompare,
        "mode"
    );
    expect_false(result.publish_allowed, "publish allowed");
    expect_false(result.reservation_allowed, "reservation allowed");
    expect_true(result.generic_verification_required, "generic verification");
    expect_false(result.authoritative, "authoritative");
    expect_false(result.approved.has_reservation(), "reservation");
    expect_equal(
        result.intent.status,
        signal::IntentStatus::PaperOpportunity,
        "intent status"
    );
    expect_true(result.output_hash != 0, "output hash");
}

void EventLocalDecisionPipeline_ExposesFastOutputHashes() {
    auto data = fixture();
    fast::EventLocalDecisionPipeline pipeline{
        &data.registry,
        config_for(data)
    };

    const auto result = pipeline.process(input_for(data));

    expect_true(result.produced_plan, "produced plan");
    expect_true(result.fast_opportunity_hash != 0, "fast opportunity hash");
    expect_true(result.fast_risk_hash != 0, "fast risk hash");
    expect_true(result.fast_plan_hash != 0, "fast plan hash");
    expect_true(result.fast_combined_hash != 0, "fast combined hash");
    expect_true(result.generic_opportunity_hash != 0, "generic opportunity hash");
    expect_true(result.generic_risk_hash != 0, "generic risk hash");
    expect_true(result.generic_plan_hash != 0, "generic plan hash");
    expect_true(result.generic_combined_hash != 0, "generic combined hash");
    expect_equal(
        result.fast_combined_hash,
        result.generic_combined_hash,
        "combined hash"
    );
    expect_equal(
        result.generic_output_hash,
        result.generic_combined_hash,
        "generic output hash"
    );
}

void EventLocalDecisionPipeline_VerifiedPaperRequiresGenericVerification() {
    auto data = fixture();
    fast::EventLocalDecisionPipeline pipeline{
        &data.registry,
        config_for(data, fast::EventLocalPipelineMode::VerifiedPaper)
    };

    const auto result = pipeline.process(input_for(data));

    expect_true(result.produced_plan, "produced plan");
    expect_false(result.fallback_required, "fallback");
    expect_equal(
        result.mode,
        fast::EventLocalPipelineMode::VerifiedPaper,
        "mode"
    );
    expect_true(result.publish_allowed, "publish allowed");
    expect_false(result.reservation_allowed, "reservation allowed");
    expect_true(result.generic_verification_required, "generic verification");
    expect_false(result.authoritative, "authoritative");
    expect_false(result.approved.has_reservation(), "reservation");
    expect_true(result.output_hash != 0, "output hash");
}

void EventLocalDecisionPipeline_PaperAuthoritativeSampleVerificationCanBeForced() {
    auto data = fixture();
    auto config = config_for(
        data,
        fast::EventLocalPipelineMode::PaperAuthoritative
    );
    config.fast_path.sample_verify_rate = 1.0;
    fast::EventLocalDecisionPipeline pipeline{&data.registry, config};

    const auto result = pipeline.process(input_for(data));

    expect_true(result.produced_plan, "produced plan");
    expect_true(result.authoritative, "authoritative");
    expect_true(result.sample_verification_required, "sample verification");
}

void FastPathConfig_DefaultsDisabled() {
    const fast::FastPathConfig config;

    expect_equal(
        config.mode,
        fast::FastPathMode::Disabled,
        "mode"
    );
    expect_false(config.enable_fixed_buy_kernel, "fixed buy kernel");
    expect_false(config.enable_simd, "simd");
    expect_equal(
        config.max_mismatches_before_disable,
        1ULL,
        "max mismatches"
    );
    expect_true(config.sample_verify_rate > 0.0, "sample verify rate");
    expect_true(config.require_kernel_spec_hash_match, "kernel hash");
    expect_true(config.require_artifact_hash_match, "artifact hash");
    expect_true(config.require_policy_hash_match, "policy hash");
}

void EventLocalDecisionPipeline_DisabledModeFallsBack() {
    auto data = fixture();
    auto config = config_for(
        data,
        fast::EventLocalPipelineMode::Disabled
    );
    fast::EventLocalDecisionPipeline pipeline{&data.registry, config};

    const auto result = pipeline.process(input_for(data));

    expect_false(result.produced_plan, "produced plan");
    expect_true(result.fallback_required, "fallback");
    expect_equal(
        result.reject_reason,
        fast::FastPathRejectReason::RuntimeDisabled,
        "reason"
    );
    expect_false(result.publish_allowed, "publish");
    expect_false(result.reservation_allowed, "reserve");
}

void EventLocalDecisionPipeline_FallbacksWhenSpecUnsupported() {
    auto data = fixture();
    data.bundle.legs[0].side = oracle::Side::Sell;
    populate_runtime_plan(&data);
    fast::EventLocalDecisionPipeline pipeline{
        &data.registry,
        config_for(data)
    };

    const auto result = pipeline.process(input_for(data));

    expect_false(result.produced_plan, "produced plan");
    expect_true(result.fallback_required, "fallback");
    expect_false(result.publish_allowed, "publish allowed");
    expect_false(result.reservation_allowed, "reservation allowed");
    expect_true(result.generic_verification_required, "generic verification");
    expect_equal(result.reject_reason, fast::FastPathRejectReason::SellLeg, "reason");
}

void EventLocalDecisionPipeline_NoPlanForLowEdgeButNoFallback() {
    auto data = fixture(600'000);
    fast::EventLocalDecisionPipeline pipeline{
        &data.registry,
        config_for(data)
    };

    const auto result = pipeline.process(input_for(data));

    expect_false(result.produced_plan, "produced plan");
    expect_false(result.fallback_required, "fallback");
    expect_equal(
        result.intent.status,
        signal::IntentStatus::RejectedLowEdge,
        "intent status"
    );
    expect_equal(
        result.decision.reject_reason,
        risk::RiskRejectReason::LowUnitEdge,
        "risk reason"
    );
    expect_true(result.output_hash != 0, "output hash");
}

void EventLocalDecisionPipeline_KillSwitchDisablesFastPath() {
    auto data = fixture();
    data.policy.kill_switch_enabled = true;
    fast::EventLocalDecisionPipeline pipeline{
        &data.registry,
        config_for(data, fast::EventLocalPipelineMode::PaperAuthoritative)
    };

    const auto result = pipeline.process(input_for(data));

    expect_false(result.produced_plan, "produced plan");
    expect_true(result.fallback_required, "fallback");
    expect_equal(
        result.reject_reason,
        fast::FastPathRejectReason::PolicyIncompatible,
        "reason"
    );
    expect_false(result.publish_allowed, "publish allowed");
    expect_false(result.reservation_allowed, "reservation allowed");
    expect_true(result.generic_verification_required, "generic verification");
}

void EventLocalDecisionPipeline_ArtifactHashRequired() {
    auto data = fixture();
    auto config = config_for(data);
    config.expected_artifact_hash ^= 0x55ULL;
    fast::EventLocalDecisionPipeline pipeline{&data.registry, config};

    const auto result = pipeline.process(input_for(data));

    expect_false(result.produced_plan, "produced plan");
    expect_true(result.fallback_required, "fallback");
    expect_equal(
        result.reject_reason,
        fast::FastPathRejectReason::ArtifactHashMismatch,
        "reason"
    );
}

void EventLocalDecisionPipeline_PolicyHashRequired() {
    auto data = fixture();
    auto config = config_for(data);
    config.expected_policy_hash ^= 0x55ULL;
    fast::EventLocalDecisionPipeline pipeline{&data.registry, config};

    const auto result = pipeline.process(input_for(data));

    expect_false(result.produced_plan, "produced plan");
    expect_true(result.fallback_required, "fallback");
    expect_equal(
        result.reject_reason,
        fast::FastPathRejectReason::PolicyHashMismatch,
        "reason"
    );
}

void EventLocalDecisionPipeline_KernelSpecHashRequired() {
    auto data = fixture();
    auto affected = data.registry.specs_for_asset(100);
    if (affected.empty()) {
        fail("expected affected spec");
    }
    const_cast<fast::FixedShapeKernelSpec&>(affected.front())
        .kernel_spec_hash ^= 0x1ULL;
    fast::EventLocalDecisionPipeline pipeline{
        &data.registry,
        config_for(data)
    };

    const auto result = pipeline.process(input_for(data));

    expect_false(result.produced_plan, "produced plan");
    expect_true(result.fallback_required, "fallback");
    expect_equal(
        result.reject_reason,
        fast::FastPathRejectReason::KernelSpecHashMismatch,
        "reason"
    );
}

void EventLocalDecisionPipeline_MismatchDisablesFastPathAndSuppressesPublish() {
    auto data = fixture();
    data.depth_views[0].prefix.ask_cum_qty[0] -= 1;
    data.depth_views[1].prefix.ask_cum_qty[0] -= 1;
    auto config = config_for(
        data,
        fast::EventLocalPipelineMode::PaperAuthoritative
    );
    config.fast_path.sample_verify_rate = 1.0;
    fast::EventLocalDecisionPipeline pipeline{&data.registry, config};

    const auto result = pipeline.process(input_for(data));

    expect_true(result.fallback_required, "fallback");
    expect_false(result.produced_plan, "produced plan");
    expect_false(result.publish_allowed, "publish");
    expect_false(result.reservation_allowed, "reserve");
    expect_true(result.generic_comparison_performed, "comparison");
    expect_true(result.mismatch, "mismatch");
    expect_true(result.fast_combined_hash != 0, "fast combined hash");
    expect_true(result.generic_combined_hash != 0, "generic combined hash");
    expect_true(!result.mismatch_reason.empty(), "mismatch reason");
    expect_equal(
        result.reject_reason,
        fast::FastPathRejectReason::RuntimeDisabled,
        "reason"
    );
    expect_true(pipeline.runtime_state().disabled, "runtime disabled");
    expect_equal(pipeline.runtime_state().mismatch_count, 1ULL, "mismatch count");
    expect_true(!pipeline.runtime_state().last_mismatch_reason.empty(), "reason");
}

void EventLocalDecisionPipeline_RuntimeDisabledAfterMismatch() {
    auto data = fixture();
    data.depth_views[0].prefix.ask_cum_qty[0] -= 1;
    data.depth_views[1].prefix.ask_cum_qty[0] -= 1;
    auto config = config_for(
        data,
        fast::EventLocalPipelineMode::PaperAuthoritative
    );
    config.fast_path.sample_verify_rate = 1.0;
    fast::EventLocalDecisionPipeline pipeline{&data.registry, config};

    (void)pipeline.process(input_for(data));
    const auto result = pipeline.process(input_for(data));

    expect_false(result.produced_plan, "produced plan");
    expect_true(result.fallback_required, "fallback");
    expect_equal(
        result.reject_reason,
        fast::FastPathRejectReason::RuntimeDisabled,
        "reason"
    );
}

void EventLocalDecisionPipeline_UsesDirtyAssetIndex() {
    auto data = fixture();
    fast::EventLocalDecisionPipeline pipeline{
        &data.registry,
        config_for(data)
    };

    const auto result = pipeline.process(input_for(data, 999));

    expect_false(result.produced_plan, "produced plan");
    expect_false(result.fallback_required, "fallback");
    expect_equal(result.intent.intent_id, 0ULL, "intent id");
    expect_true(result.output_hash != 0, "output hash");
}

void EventLocalDecisionPipeline_OutputHashDeterministic() {
    auto data = fixture();
    fast::EventLocalDecisionPipeline pipeline{
        &data.registry,
        config_for(data)
    };

    const auto first = pipeline.process(input_for(data));
    const auto second = pipeline.process(input_for(data));

    expect_equal(first.output_hash, second.output_hash, "output hash");
    expect_equal(first.intent.intent_id, second.intent.intent_id, "intent id");
    expect_equal(first.plan.plan_id, second.plan.plan_id, "plan id");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"EventLocalDecisionPipeline_ProducesPlanForEligibleSpec",
         &EventLocalDecisionPipeline_ProducesPlanForEligibleSpec},
        {"EventLocalDecisionPipeline_ShadowCompareDoesNotPublishOrReserve",
         &EventLocalDecisionPipeline_ShadowCompareDoesNotPublishOrReserve},
        {"EventLocalDecisionPipeline_ExposesFastOutputHashes",
         &EventLocalDecisionPipeline_ExposesFastOutputHashes},
        {"EventLocalDecisionPipeline_VerifiedPaperRequiresGenericVerification",
         &EventLocalDecisionPipeline_VerifiedPaperRequiresGenericVerification},
        {"EventLocalDecisionPipeline_PaperAuthoritativeSampleVerificationCanBeForced",
         &EventLocalDecisionPipeline_PaperAuthoritativeSampleVerificationCanBeForced},
        {"EventLocalDecisionPipeline_FallbacksWhenSpecUnsupported",
         &EventLocalDecisionPipeline_FallbacksWhenSpecUnsupported},
        {"EventLocalDecisionPipeline_NoPlanForLowEdgeButNoFallback",
         &EventLocalDecisionPipeline_NoPlanForLowEdgeButNoFallback},
        {"EventLocalDecisionPipeline_UsesDirtyAssetIndex",
         &EventLocalDecisionPipeline_UsesDirtyAssetIndex},
        {"EventLocalDecisionPipeline_OutputHashDeterministic",
         &EventLocalDecisionPipeline_OutputHashDeterministic},
        {"EventLocalDecisionPipeline_KillSwitchDisablesFastPath",
         &EventLocalDecisionPipeline_KillSwitchDisablesFastPath},
        {"FastPathConfig_DefaultsDisabled",
         &FastPathConfig_DefaultsDisabled},
        {"EventLocalDecisionPipeline_DisabledModeFallsBack",
         &EventLocalDecisionPipeline_DisabledModeFallsBack},
        {"EventLocalDecisionPipeline_ArtifactHashRequired",
         &EventLocalDecisionPipeline_ArtifactHashRequired},
        {"EventLocalDecisionPipeline_PolicyHashRequired",
         &EventLocalDecisionPipeline_PolicyHashRequired},
        {"EventLocalDecisionPipeline_KernelSpecHashRequired",
         &EventLocalDecisionPipeline_KernelSpecHashRequired},
        {"EventLocalDecisionPipeline_MismatchDisablesFastPathAndSuppressesPublish",
         &EventLocalDecisionPipeline_MismatchDisablesFastPathAndSuppressesPublish},
        {"EventLocalDecisionPipeline_RuntimeDisabledAfterMismatch",
         &EventLocalDecisionPipeline_RuntimeDisabledAfterMismatch},
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
