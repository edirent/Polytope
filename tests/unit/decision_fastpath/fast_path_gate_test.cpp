#include "engine/decision_fastpath/core/FastPathGate.h"

#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/signal/pricing/SideResolver.h"
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

struct Fixture {
    oracle::CandidateBundle bundle;
    signal::BundleRuntimePlan plan;
    risk::RiskPolicySnapshot policy;
    std::array<state::MarketDepthView, signal::kMaxIntentLegs> depth_views{};
    std::uint16_t depth_count = 0;
    std::uint64_t universe_version = 42;
};

state::MarketDepthView depth_view(std::uint32_t asset_index) {
    state::MarketDepthView view;
    view.asset_index = asset_index;
    view.book_version = 10 + asset_index;
    view.snapshot_version_hash = 100 + asset_index;
    view.last_ws_recv_ns = 1'000;
    view.usable_for_depth = true;
    view.ask_count = 1;
    view.asks[0] = state::PriceLevel{
        .price_tick = 500'000,
        .price = 0.50,
        .size = 100.0
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

Fixture fixture(std::uint16_t leg_count = 2) {
    Fixture out;
    out.bundle.bundle_id = 17;
    out.bundle.leg_count = leg_count;
    out.bundle.guaranteed_payout_tick = 1'000'000;

    out.plan.bundle = &out.bundle;
    out.plan.bundle_id = out.bundle.bundle_id;
    out.plan.bundle_hash = 7001;
    out.plan.oracle_artifact_hash = 9001;
    out.plan.constraint_hash = 8001;
    out.plan.leg_count = leg_count;
    out.plan.unique_asset_count = leg_count;

    for (std::uint16_t i = 0; i < leg_count; ++i) {
        out.bundle.legs[i].market_id = "m" + std::to_string(i);
        out.bundle.legs[i].asset_id = "asset-" + std::to_string(i);
        out.bundle.legs[i].side = oracle::Side::Buy;
        out.bundle.legs[i].quantity_lots = 1;
        out.bundle.legs[i].max_price_tick = 1'000'000;

        out.plan.market_ids[i] = &out.bundle.legs[i].market_id;
        out.plan.asset_ids[i] = &out.bundle.legs[i].asset_id;
        out.plan.asset_indices[i] = 100 + i;
        out.plan.sides[i] = oracle::Side::Buy;
        out.plan.executable_sides[i] = signal::ExecutableBookSide::Asks;
        out.plan.ratio_qty_lots[i] = 1;
        out.plan.max_price_ticks[i] = 1'000'000;
        out.plan.unique_asset_ids[i] = &out.bundle.legs[i].asset_id;
        out.plan.unique_asset_indices[i] = 100 + i;
        out.depth_views[i] = depth_view(100 + i);
    }
    out.depth_count = leg_count;

    out.policy = risk::with_computed_policy_hash(risk::RiskPolicySnapshot{});
    return out;
}

fast::FastPathGateInput input_for(Fixture& data) {
    return fast::FastPathGateInput{
        .plan = &data.plan,
        .depth_views = std::span<const state::MarketDepthView>{
            data.depth_views.data(),
            data.depth_count
        },
        .policy = &data.policy,
        .expected_oracle_artifact_hash = data.plan.oracle_artifact_hash,
        .expected_policy_hash = data.policy.policy_hash,
        .expected_universe_version = data.universe_version,
        .current_universe_version = data.universe_version,
        .requires_dynamic_runtime_oracle_check = false,
        .settlement_masks_available = true,
        .max_supported_legs = 16
    };
}

void FastPathGate_AcceptsBuyOnlyFixedShape() {
    auto data = fixture();
    fast::FastPathGate gate;

    const auto result = gate.evaluate(input_for(data));

    expect_true(result.eligible, "eligible");
    expect_equal(result.reason, fast::FastPathRejectReason::None, "reason");
    expect_equal(gate.stats().evaluated, 1ULL, "evaluated");
    expect_equal(gate.stats().eligible, 1ULL, "eligible count");
    expect_equal(gate.stats().fallback, 0ULL, "fallback count");
}

void FastPathGate_RejectsSellLeg() {
    auto data = fixture();
    data.plan.sides[1] = oracle::Side::Sell;
    fast::FastPathGate gate;

    const auto result = gate.evaluate(input_for(data));

    expect_false(result.eligible, "eligible");
    expect_equal(result.reason, fast::FastPathRejectReason::SellLeg, "reason");
    expect_equal(gate.stats().fallback, 1ULL, "fallback count");
    expect_equal(
        gate.stats().by_reason[
            static_cast<std::size_t>(fast::FastPathRejectReason::SellLeg)
        ],
        1ULL,
        "reason count"
    );
}

void FastPathGate_RejectsDynamicOracleBundle() {
    auto data = fixture();
    auto input = input_for(data);
    input.requires_dynamic_runtime_oracle_check = true;
    fast::FastPathGate gate;

    const auto result = gate.evaluate(input);

    expect_false(result.eligible, "eligible");
    expect_equal(
        result.reason,
        fast::FastPathRejectReason::DynamicRuntimeOracleRequired,
        "reason"
    );
}

void FastPathGate_RejectsMissingDepthView() {
    auto data = fixture();
    data.depth_count = 1;
    fast::FastPathGate gate;

    const auto result = gate.evaluate(input_for(data));

    expect_false(result.eligible, "eligible");
    expect_equal(
        result.reason,
        fast::FastPathRejectReason::MissingDepthView,
        "reason"
    );
}

void FastPathGate_RejectsPolicyIncompatible() {
    auto data = fixture();
    data.policy.kill_switch_enabled = true;
    data.policy = risk::with_computed_policy_hash(data.policy);
    fast::FastPathGate gate;

    const auto result = gate.evaluate(input_for(data));

    expect_false(result.eligible, "eligible");
    expect_equal(
        result.reason,
        fast::FastPathRejectReason::PolicyIncompatible,
        "reason"
    );
}

void FastPathGate_RejectsMismatchedArtifactHash() {
    auto data = fixture();
    auto input = input_for(data);
    input.expected_oracle_artifact_hash = data.plan.oracle_artifact_hash + 1;
    fast::FastPathGate gate;

    const auto result = gate.evaluate(input);

    expect_false(result.eligible, "eligible");
    expect_equal(
        result.reason,
        fast::FastPathRejectReason::ArtifactHashMismatch,
        "reason"
    );
}

void FastPathGate_RejectsMissingSettlementMaskDependency() {
    auto data = fixture();
    data.bundle.required_true_mask = 1;
    auto input = input_for(data);
    input.settlement_masks_available = false;
    fast::FastPathGate gate;

    const auto result = gate.evaluate(input);

    expect_false(result.eligible, "eligible");
    expect_equal(
        result.reason,
        fast::FastPathRejectReason::MissingSettlementMaskDependency,
        "reason"
    );
}

void FastPathGate_RejectsBadDepthView() {
    auto data = fixture();
    data.depth_views[0].usable_for_depth = false;
    fast::FastPathGate gate;

    const auto result = gate.evaluate(input_for(data));

    expect_false(result.eligible, "eligible");
    expect_equal(
        result.reason,
        fast::FastPathRejectReason::BadDepthView,
        "reason"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"FastPathGate_AcceptsBuyOnlyFixedShape",
         &FastPathGate_AcceptsBuyOnlyFixedShape},
        {"FastPathGate_RejectsSellLeg", &FastPathGate_RejectsSellLeg},
        {"FastPathGate_RejectsDynamicOracleBundle",
         &FastPathGate_RejectsDynamicOracleBundle},
        {"FastPathGate_RejectsMissingDepthView",
         &FastPathGate_RejectsMissingDepthView},
        {"FastPathGate_RejectsPolicyIncompatible",
         &FastPathGate_RejectsPolicyIncompatible},
        {"FastPathGate_RejectsMismatchedArtifactHash",
         &FastPathGate_RejectsMismatchedArtifactHash},
        {"FastPathGate_RejectsMissingSettlementMaskDependency",
         &FastPathGate_RejectsMissingSettlementMaskDependency},
        {"FastPathGate_RejectsBadDepthView",
         &FastPathGate_RejectsBadDepthView},
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
