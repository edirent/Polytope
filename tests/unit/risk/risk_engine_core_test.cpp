#include "engine/risk/core/RiskEngine.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using trading_engine::risk::RiskDecisionStatus;
using trading_engine::risk::RiskEngine;
using trading_engine::risk::RiskEvaluationContext;
using trading_engine::risk::RiskRejectReason;
using trading_engine::risk::RiskPolicySnapshot;
using trading_engine::signal::IntentStatus;
using trading_engine::signal::OpportunityIntent;
using trading_engine::state::MarketStateSnapshot;

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

MarketStateSnapshot snapshot(
    std::string asset_id,
    std::int64_t ask_price_tick,
    double ask_size,
    std::uint64_t now_ns = 1'500
) {
    MarketStateSnapshot out;
    out.entity_id = std::move(asset_id);
    out.market_id = "market";
    out.version = 7;
    out.last_book_update_ns = now_ns - 100;
    out.live = true;
    out.has_ask = true;
    out.ask_count = 1;
    out.asks[0].price_tick = ask_price_tick;
    out.asks[0].size = ask_size;
    out.usable_for_depth = true;
    out.state_hash = 111;
    return out;
}

OpportunityIntent intent(
    std::string key = "intent-key",
    std::string asset_id = "asset-1",
    std::int64_t estimated_cost_tick = 8'000,
    std::int64_t bundle_qty = 10
) {
    OpportunityIntent out;
    out.intent_id = 100;
    out.bundle_id = 200;
    out.status = IntentStatus::PaperOpportunity;
    out.valid_under_settlement = true;
    out.passed_quality_gate = true;
    out.enough_depth = true;
    out.guaranteed_payout_tick = 20'000;
    out.estimated_cost_tick = estimated_cost_tick;
    out.estimated_fee_tick = 0;
    out.latency_buffer_tick = 0;
    out.estimated_edge_tick = 12'000;
    out.oracle_artifact_hash = 123;
    out.bundle_hash = 456;
    out.snapshot_version = 7;
    out.snapshot_version_hash = 111;
    out.bundle_qty = bundle_qty;
    out.unit_edge_tick = 1'200;
    out.total_edge_tick = 12'000;
    out.created_ts_ns = 1'000;
    out.expires_at_ns = 2'000;
    out.idempotency_key = std::move(key);
    out.leg_count = 1;
    out.legs[0].market_id = "market";
    out.legs[0].asset_id = std::move(asset_id);
    out.legs[0].quantity_lots = 1;
    out.legs[0].estimated_cost_tick = estimated_cost_tick;
    return out;
}

RiskEvaluationContext context(
    RiskPolicySnapshot policy = {},
    std::vector<MarketStateSnapshot> snapshots = {snapshot("asset-1", 800, 100.0)}
) {
    RiskEvaluationContext out;
    out.now_ns = 1'500;
    out.latest_snapshots = std::move(snapshots);
    out.policy = policy;
    return out;
}

void RiskEngine_ApprovesSafeIntentAndCreatesReservation() {
    RiskEngine engine;

    const auto result = engine.evaluate(intent(), context());

    expect_true(result.decision.approved(), "approved");
    expect_true(result.approved_intent.valid(), "approved intent valid");
    expect_true(result.reservation.ok, "reservation ok");
    expect_true(result.reservation.reservation_id != 0, "reservation id");
    expect_true(result.reservation_attempted, "reservation attempted");
    expect_equal(engine.ledger_snapshot().active_reservations, 1ULL, "active");
}

void RiskEngine_RejectsExpiredBeforeVWAP() {
    RiskEngine engine;
    auto expired = intent("expired");
    expired.expires_at_ns = 1'500;

    const auto result = engine.evaluate(expired, context());

    expect_false(result.decision.approved(), "approved");
    expect_equal(
        result.decision.reject_reason,
        RiskRejectReason::ExpiredIntent,
        "reject"
    );
    expect_false(result.cost_revalidated, "cost revalidated");
    expect_false(result.reservation_attempted, "reservation attempted");
    expect_equal(engine.ledger_snapshot().active_reservations, 0ULL, "active");
}

void RiskEngine_RejectsDuplicateBeforeVWAP() {
    RiskEngine engine;
    const auto first = engine.evaluate(intent("same"), context());
    expect_true(first.decision.approved(), "first approved");

    const auto second = engine.evaluate(intent("same"), context());

    expect_false(second.decision.approved(), "approved");
    expect_equal(
        second.decision.reject_reason,
        RiskRejectReason::DuplicateIntent,
        "reject"
    );
    expect_false(second.cost_revalidated, "cost revalidated");
    expect_false(second.reservation_attempted, "reservation attempted");
    expect_equal(engine.ledger_snapshot().active_reservations, 1ULL, "active");
}

void RiskEngine_RejectsStaleBook() {
    RiskEngine engine;
    RiskPolicySnapshot policy;
    policy.max_book_age_ns = 50;

    auto stale_snapshot = snapshot("asset-1", 800, 100.0);
    stale_snapshot.last_book_update_ns = 1'000;

    const auto result =
        engine.evaluate(intent("stale"), context(policy, {stale_snapshot}));

    expect_false(result.decision.approved(), "approved");
    expect_equal(
        result.decision.reject_reason,
        RiskRejectReason::StaleBook,
        "reject"
    );
    expect_false(result.cost_revalidated, "cost revalidated");
    expect_false(result.reservation_attempted, "reservation attempted");
}

void RiskEngine_RejectsCostDrift() {
    RiskEngine engine;
    RiskPolicySnapshot policy;
    policy.max_allowed_cost_drift_tick = 500;

    const auto result = engine.evaluate(
        intent("drift"),
        context(policy, {snapshot("asset-1", 1'000, 100.0)})
    );

    expect_false(result.decision.approved(), "approved");
    expect_equal(
        result.decision.reject_reason,
        RiskRejectReason::CostDrift,
        "reject"
    );
    expect_true(result.cost_revalidated, "cost revalidated");
    expect_false(result.reservation_attempted, "reservation attempted");
}

void RiskEngine_RejectsExposureLimit() {
    RiskEngine engine;
    auto first = engine.evaluate(intent("first"), context());
    expect_true(first.decision.approved(), "first approved");

    RiskPolicySnapshot policy;
    policy.max_total_exposure_tick = 10'000;

    const auto second = engine.evaluate(
        intent("second", "asset-2", 3'000, 10),
        context(policy, {snapshot("asset-2", 300, 100.0)})
    );

    expect_false(second.decision.approved(), "approved");
    expect_equal(
        second.decision.reject_reason,
        RiskRejectReason::TotalExposureLimit,
        "reject"
    );
    expect_true(second.cost_revalidated, "cost revalidated");
    expect_false(second.reservation_attempted, "reservation attempted");
    expect_equal(engine.ledger_snapshot().active_reservations, 1ULL, "active");
}

void RiskEngine_RejectsPartialFillRisk() {
    RiskEngine engine;
    RiskPolicySnapshot policy;
    policy.min_depth_margin_ratio = 1.20;

    auto multi = intent("partial", "asset-1", 200, 1);
    multi.leg_count = 2;
    multi.guaranteed_payout_tick = 2'000;
    multi.estimated_cost_tick = 200;
    multi.total_edge_tick = 1'800;
    multi.legs[0].asset_id = "asset-1";
    multi.legs[0].quantity_lots = 10;
    multi.legs[0].estimated_cost_tick = 100;
    multi.legs[1].market_id = "market-2";
    multi.legs[1].asset_id = "asset-2";
    multi.legs[1].quantity_lots = 10;
    multi.legs[1].estimated_cost_tick = 100;

    const auto result = engine.evaluate(
        multi,
        context(
            policy,
            {snapshot("asset-1", 10, 10.0), snapshot("asset-2", 10, 10.0)}
        )
    );

    expect_false(result.decision.approved(), "approved");
    expect_equal(
        result.decision.reject_reason,
        RiskRejectReason::PartialFillRisk,
        "reject"
    );
    expect_true(result.cost_revalidated, "cost revalidated");
    expect_false(result.reservation_attempted, "reservation attempted");
}

void RiskEngine_KillSwitchRejectsAll() {
    RiskEngine engine;
    RiskPolicySnapshot policy;
    policy.kill_switch_enabled = true;

    const auto result = engine.evaluate(intent("kill"), context(policy));

    expect_false(result.decision.approved(), "approved");
    expect_equal(
        result.decision.reject_reason,
        RiskRejectReason::KillSwitch,
        "reject"
    );
    expect_false(result.cost_revalidated, "cost revalidated");
    expect_false(result.reservation_attempted, "reservation attempted");
}

void RiskEngine_OutputHashDeterministic() {
    RiskEngine engine_a;
    RiskEngine engine_b;

    const auto a = engine_a.evaluate(intent("stable"), context());
    const auto b = engine_b.evaluate(intent("stable"), context());

    expect_true(a.decision.approved(), "a approved");
    expect_true(b.decision.approved(), "b approved");
    expect_true(a.output_hash != 0, "hash nonzero");
    expect_equal(a.output_hash, b.output_hash, "hash stable");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"RiskEngine_ApprovesSafeIntentAndCreatesReservation", &RiskEngine_ApprovesSafeIntentAndCreatesReservation},
        {"RiskEngine_RejectsExpiredBeforeVWAP", &RiskEngine_RejectsExpiredBeforeVWAP},
        {"RiskEngine_RejectsDuplicateBeforeVWAP", &RiskEngine_RejectsDuplicateBeforeVWAP},
        {"RiskEngine_RejectsStaleBook", &RiskEngine_RejectsStaleBook},
        {"RiskEngine_RejectsCostDrift", &RiskEngine_RejectsCostDrift},
        {"RiskEngine_RejectsExposureLimit", &RiskEngine_RejectsExposureLimit},
        {"RiskEngine_RejectsPartialFillRisk", &RiskEngine_RejectsPartialFillRisk},
        {"RiskEngine_KillSwitchRejectsAll", &RiskEngine_KillSwitchRejectsAll},
        {"RiskEngine_OutputHashDeterministic", &RiskEngine_OutputHashDeterministic}
    };
    return test_map;
}

int run_test(const std::string& name) {
    const auto it = tests().find(name);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << name << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << name << " failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << name << " passed\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        return run_test(argv[1]);
    }

    int failures = 0;
    for (const auto& [name, _] : tests()) {
        failures += run_test(name) == 0 ? 0 : 1;
    }
    return failures == 0 ? 0 : 1;
}
