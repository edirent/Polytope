#include "engine/decision_fastpath/kernel/FixedBuyBundleKernelScalar.h"

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

fast::FixedShapeKernelSpec spec() {
    fast::FixedShapeKernelSpec out;
    out.artifact_hash = 9'001;
    out.constraint_hash = 8'001;
    out.bundle_hash = 7'001;
    out.bundle_id = 17;
    out.leg_count = 2;
    out.asset_indices[0] = 100;
    out.asset_indices[1] = 101;
    out.market_indices[0] = 200;
    out.market_indices[1] = 201;
    out.sides[0] = oracle::Side::Buy;
    out.sides[1] = oracle::Side::Buy;
    out.ratio_qty_lots[0] = 1;
    out.ratio_qty_lots[1] = 1;
    out.guaranteed_payout_tick = 1'000'000;
    out.min_unit_edge_tick = 1;
    out.min_total_edge_tick = 1;
    out.min_edge_bps = 0;
    out.min_bundle_qty = 1;
    out.kernel_spec_hash = fast::hash_fixed_shape_kernel_spec(out);
    return out;
}

risk::RiskPolicySnapshot policy() {
    return risk::with_computed_policy_hash(risk::RiskPolicySnapshot{});
}

struct Fixture {
    fast::FixedShapeKernelSpec kernel_spec = spec();
    risk::RiskPolicySnapshot risk_policy = policy();
    risk::RiskLedgerSnapshot ledger;
    std::array<state::MarketDepthView, 2> depth_views{
        depth_view(100, 400'000, 10.0),
        depth_view(101, 400'000, 10.0),
    };
};

fast::FastKernelResult run(Fixture& data) {
    fast::FixedBuyBundleKernelScalar kernel;
    fast::FastPathScratch scratch;
    return kernel.run(
        data.kernel_spec,
        data.depth_views.data(),
        static_cast<std::uint16_t>(data.depth_views.size()),
        data.risk_policy,
        data.ledger,
        2'000,
        &scratch
    );
}

void FixedBuyBundleKernelScalar_ProducesPlan() {
    Fixture data;

    const auto result = run(data);

    expect_true(result.produced_intent, "intent");
    expect_true(result.produced_plan, "plan");
    expect_false(result.fallback_required, "fallback");
    expect_true(result.decision.approved(), "approved");
    expect_equal(result.intent.bundle_qty, 10LL, "bundle qty");
    expect_equal(result.intent.estimated_cost_tick, 8'000'000LL, "total cost");
    expect_equal(result.intent.unit_edge_tick, 200'000LL, "unit edge");
    expect_equal(result.intent.total_edge_tick, 2'000'000LL, "total edge");
    expect_equal(result.plan.order_count, static_cast<std::uint16_t>(2), "orders");
    expect_equal(result.plan.orders[0].quantity_lots, 10LL, "order qty");
    expect_true(result.output_hash != 0, "hash");
}

void FixedBuyBundleKernelScalar_RejectsSellLeg() {
    Fixture data;
    data.kernel_spec.sides[1] = oracle::Side::Sell;

    const auto result = run(data);

    expect_true(result.fallback_required, "fallback");
    expect_equal(result.reject_reason, fast::FastPathRejectReason::SellLeg, "reason");
}

void FixedBuyBundleKernelScalar_RejectsMissingDepth() {
    Fixture data;
    fast::FixedBuyBundleKernelScalar kernel;

    const auto result = kernel.run(
        data.kernel_spec,
        data.depth_views.data(),
        1,
        data.risk_policy,
        data.ledger,
        2'000
    );

    expect_true(result.fallback_required, "fallback");
    expect_equal(
        result.reject_reason,
        fast::FastPathRejectReason::MissingDepthView,
        "reason"
    );
}

void FixedBuyBundleKernelScalar_RejectsBadDepthFlags() {
    Fixture data;
    data.depth_views[0].crossed = true;

    const auto result = run(data);

    expect_true(result.fallback_required, "fallback");
    expect_equal(result.reject_reason, fast::FastPathRejectReason::BadDepthView, "reason");
}

void FixedBuyBundleKernelScalar_LowEdgeDoesNotProducePlan() {
    Fixture data;
    data.depth_views[0] = depth_view(100, 600'000, 10.0);
    data.depth_views[1] = depth_view(101, 600'000, 10.0);

    const auto result = run(data);

    expect_true(result.produced_intent, "intent");
    expect_false(result.produced_plan, "plan");
    expect_false(result.fallback_required, "fallback");
    expect_equal(
        result.decision.reject_reason,
        risk::RiskRejectReason::LowUnitEdge,
        "risk reason"
    );
}

void FixedBuyBundleKernelScalar_RejectsTotalExposureLimit() {
    Fixture data;
    data.risk_policy.max_total_exposure_tick = 10'000'000;
    data.risk_policy = risk::with_computed_policy_hash(data.risk_policy);
    data.ledger.total_reserved_exposure_tick = 3'000'001;

    const auto result = run(data);

    expect_true(result.produced_intent, "intent");
    expect_false(result.produced_plan, "plan");
    expect_equal(
        result.decision.reject_reason,
        risk::RiskRejectReason::TotalExposureLimit,
        "risk reason"
    );
}

void FixedBuyBundleKernelScalar_RejectsNumericInventoryLimit() {
    Fixture data;
    data.risk_policy.max_inventory_lots_per_asset = 12;
    data.risk_policy = risk::with_computed_policy_hash(data.risk_policy);
    data.ledger.numeric_asset_count = 1;
    data.ledger.numeric_reserved_asset_lots[0] = {
        .asset_index = 100,
        .lots = 3
    };

    const auto result = run(data);

    expect_true(result.produced_intent, "intent");
    expect_false(result.produced_plan, "plan");
    expect_equal(
        result.decision.reject_reason,
        risk::RiskRejectReason::InventoryLimit,
        "risk reason"
    );
}

void FixedBuyBundleKernelScalar_DoesNotMaterializeStrings() {
    Fixture data;

    const auto result = run(data);

    expect_true(result.produced_plan, "plan");
    expect_true(result.intent.idempotency_key.empty(), "idempotency key");
    expect_true(result.intent.legs[0].asset_id.empty(), "asset string");
    expect_true(result.plan.idempotency_key.empty(), "plan idem key");
    expect_true(result.plan.orders[0].client_order_id.empty(), "client order id");
}

void FixedBuyBundleKernelScalar_OutputHashDeterministic() {
    Fixture data;

    const auto first = run(data);
    const auto second = run(data);

    expect_equal(first.output_hash, second.output_hash, "hash");
    expect_equal(first.intent.intent_id, second.intent.intent_id, "intent");
    expect_equal(first.plan.plan_id, second.plan.plan_id, "plan");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"FixedBuyBundleKernelScalar_ProducesPlan",
         &FixedBuyBundleKernelScalar_ProducesPlan},
        {"FixedBuyBundleKernelScalar_RejectsSellLeg",
         &FixedBuyBundleKernelScalar_RejectsSellLeg},
        {"FixedBuyBundleKernelScalar_RejectsMissingDepth",
         &FixedBuyBundleKernelScalar_RejectsMissingDepth},
        {"FixedBuyBundleKernelScalar_RejectsBadDepthFlags",
         &FixedBuyBundleKernelScalar_RejectsBadDepthFlags},
        {"FixedBuyBundleKernelScalar_LowEdgeDoesNotProducePlan",
         &FixedBuyBundleKernelScalar_LowEdgeDoesNotProducePlan},
        {"FixedBuyBundleKernelScalar_RejectsTotalExposureLimit",
         &FixedBuyBundleKernelScalar_RejectsTotalExposureLimit},
        {"FixedBuyBundleKernelScalar_RejectsNumericInventoryLimit",
         &FixedBuyBundleKernelScalar_RejectsNumericInventoryLimit},
        {"FixedBuyBundleKernelScalar_DoesNotMaterializeStrings",
         &FixedBuyBundleKernelScalar_DoesNotMaterializeStrings},
        {"FixedBuyBundleKernelScalar_OutputHashDeterministic",
         &FixedBuyBundleKernelScalar_OutputHashDeterministic},
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
