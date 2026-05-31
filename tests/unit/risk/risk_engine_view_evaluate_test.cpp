#include "engine/risk/core/RiskEngine.h"

#include "engine/risk/public/RiskInputView.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "engine/state/MarketStateSnapshot.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::risk::RiskEngine;
using trading_engine::risk::RiskInputView;
using trading_engine::risk::RiskPolicySnapshot;
using trading_engine::risk::RiskRejectReason;
using trading_engine::risk::RiskRuntimeContext;
using trading_engine::risk::RiskVWAPMode;
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

std::uint64_t idempotency_hash_for(const std::string& key) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char ch : key) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1 : hash;
}

MarketStateSnapshot snapshot(
    std::uint64_t now_ns = 1'500,
    std::uint64_t snapshot_hash = 111
) {
    MarketStateSnapshot out;
    out.entity_id = "asset-1";
    out.market_id = "market-1";
    out.version = 7;
    out.last_book_update_ns = now_ns - 100;
    out.live = true;
    out.has_ask = true;
    out.ask_count = 1;
    out.asks[0].price_tick = 800;
    out.asks[0].size = 100.0;
    out.usable_for_depth = true;
    out.snapshot_version_hash = snapshot_hash;
    out.state_hash = snapshot_hash;
    return out;
}

OpportunityIntent intent(std::string key = "risk-view") {
    OpportunityIntent out;
    out.intent_id = 100;
    out.bundle_id = 200;
    out.status = IntentStatus::PaperOpportunity;
    out.valid_under_settlement = true;
    out.passed_quality_gate = true;
    out.enough_depth = true;
    out.guaranteed_payout_tick = 20'000;
    out.estimated_cost_tick = 8'000;
    out.estimated_edge_tick = 12'000;
    out.oracle_artifact_hash = 123;
    out.constraint_hash = 234;
    out.bundle_hash = 456;
    out.snapshot_version = 7;
    out.snapshot_version_hash = 111;
    out.idempotency_hash = idempotency_hash_for(key);
    out.bundle_qty = 10;
    out.original_bundle_qty = 10;
    out.unit_edge_tick = 1'200;
    out.total_edge_tick = 12'000;
    out.created_ts_ns = 1'000;
    out.expires_at_ns = 2'000;
    out.idempotency_key = std::move(key);
    out.leg_count = 1;
    out.legs[0].market_id = "market-1";
    out.legs[0].asset_id = "asset-1";
    out.legs[0].quantity_lots = 1;
    out.legs[0].estimated_cost_tick = 8'000;
    out.legs[0].requested_qty_lots = 10;
    out.legs[0].executable_qty_lots = 12;
    out.legs[0].depth_margin_bps = 12'000;
    out.legs[0].enough_depth = true;
    return out;
}

RiskPolicySnapshot permissive_policy() {
    RiskPolicySnapshot policy;
    policy.max_allowed_cost_drift_tick = 1'000'000;
    policy.max_total_cost_tick = 1'000'000;
    policy.max_single_market_exposure_tick = 1'000'000;
    policy.max_total_exposure_tick = 1'000'000;
    policy.max_inventory_lots_per_asset = 1'000'000;
    return policy;
}

RiskInputView view_for(
    const OpportunityIntent& in,
    const MarketStateSnapshot& snap,
    const RiskPolicySnapshot& policy,
    std::uint64_t snapshot_version_hash = 111,
    std::uint64_t now_ns = 1'500
) {
    RiskInputView view;
    view.intent = &in;
    view.snapshots = &snap;
    view.snapshot_count = 1;
    view.snapshot_version_hash = snapshot_version_hash;
    view.now_ns = now_ns;
    view.policy = &policy;
    return view;
}

RiskEngine engine_with_policy(const RiskPolicySnapshot& policy) {
    RiskRuntimeContext runtime;
    runtime.policy = &policy;
    return RiskEngine(runtime);
}

void RiskEngine_EvaluateViewAcceptsValidInput() {
    const auto policy = permissive_policy();
    auto engine = engine_with_policy(policy);
    const auto in = intent();
    const auto snap = snapshot();

    const auto result = engine.evaluate(view_for(in, snap, policy));

    expect_true(result.decision.approved(), "approved");
    expect_equal(
        result.cost.vwap_mode,
        RiskVWAPMode::ReuseSignalSnapshot,
        "vwap mode"
    );
}

void RiskEngine_EvaluateViewRejectsExpiredIntent() {
    const auto policy = permissive_policy();
    auto engine = engine_with_policy(policy);
    auto in = intent("expired-view");
    in.expires_at_ns = 1'500;
    const auto snap = snapshot();

    const auto result = engine.evaluate(view_for(in, snap, policy));

    expect_false(result.decision.approved(), "approved");
    expect_equal(
        result.decision.reject_reason,
        RiskRejectReason::ExpiredIntent,
        "reject reason"
    );
    expect_false(result.cost_revalidated, "cost revalidated");
}

void RiskEngine_EvaluateViewRejectsStaleSnapshots() {
    auto policy = permissive_policy();
    policy.max_book_age_ns = 100;
    auto engine = engine_with_policy(policy);
    const auto in = intent("stale-view");
    auto snap = snapshot();
    snap.last_book_update_ns = 1'000;

    const auto result = engine.evaluate(view_for(in, snap, policy, 111, 1'500));

    expect_false(result.decision.approved(), "approved");
    expect_equal(
        result.decision.reject_reason,
        RiskRejectReason::StaleBook,
        "reject reason"
    );
}

void RiskEngine_EvaluateViewFastPathNoSnapshotRequery() {
    const auto policy = permissive_policy();
    auto engine = engine_with_policy(policy);
    const auto in = intent("fast-path");
    const auto snap = snapshot();

    const auto result = engine.evaluate(view_for(in, snap, policy, 111));

    expect_true(result.decision.approved(), "approved");
    expect_equal(
        result.cost.vwap_mode,
        RiskVWAPMode::ReuseSignalSnapshot,
        "vwap mode"
    );
    expect_equal(
        result.result.metrics.snapshot_fast_path,
        1ULL,
        "fast path metric"
    );
    expect_equal(
        result.result.metrics.snapshot_requery,
        0ULL,
        "requery metric"
    );
}

void RiskEngine_EvaluateViewFallbackRequeryOnHashMismatch() {
    const auto policy = permissive_policy();
    auto engine = engine_with_policy(policy);
    const auto in = intent("fallback-path");
    const auto snap = snapshot();

    const auto result = engine.evaluate(view_for(in, snap, policy, 999));

    expect_true(result.decision.approved(), "approved");
    expect_equal(
        result.cost.vwap_mode,
        RiskVWAPMode::RecomputedFromSnapshot,
        "vwap mode"
    );
    expect_equal(
        result.result.metrics.snapshot_fast_path,
        0ULL,
        "fast path metric"
    );
    expect_equal(
        result.result.metrics.snapshot_requery,
        1ULL,
        "requery metric"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "RiskEngine_EvaluateViewAcceptsValidInput",
            &RiskEngine_EvaluateViewAcceptsValidInput
        },
        {
            "RiskEngine_EvaluateViewRejectsExpiredIntent",
            &RiskEngine_EvaluateViewRejectsExpiredIntent
        },
        {
            "RiskEngine_EvaluateViewRejectsStaleSnapshots",
            &RiskEngine_EvaluateViewRejectsStaleSnapshots
        },
        {
            "RiskEngine_EvaluateViewFastPathNoSnapshotRequery",
            &RiskEngine_EvaluateViewFastPathNoSnapshotRequery
        },
        {
            "RiskEngine_EvaluateViewFallbackRequeryOnHashMismatch",
            &RiskEngine_EvaluateViewFallbackRequeryOnHashMismatch
        },
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
    } catch (const std::exception& ex) {
        std::cerr << argv[1] << " failed: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
