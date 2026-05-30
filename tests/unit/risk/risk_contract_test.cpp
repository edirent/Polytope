#include "engine/risk/public/ApprovedIntent.h"
#include "engine/risk/public/RiskAuditTrace.h"
#include "engine/risk/public/RiskConfig.h"
#include "engine/risk/public/RiskDecision.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/risk/public/RiskResult.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::risk::ApprovedIntent;
using trading_engine::risk::RiskAuditTrace;
using trading_engine::risk::RiskConfig;
using trading_engine::risk::RiskDecision;
using trading_engine::risk::RiskDecisionStatus;
using trading_engine::risk::RiskPolicySnapshot;
using trading_engine::risk::RiskRejectReason;
using trading_engine::risk::RiskResult;
using trading_engine::risk::compute_policy_hash;
using trading_engine::risk::make_approved_decision;
using trading_engine::risk::with_computed_policy_hash;

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

void RiskPolicySnapshot_DefaultsAreSafe() {
    const RiskPolicySnapshot policy;

    expect_equal(policy.policy_version, 1ULL, "policy_version");
    expect_equal(policy.policy_hash, 0ULL, "policy_hash");
    expect_true(policy.risk_enabled, "risk_enabled");
    expect_false(policy.kill_switch_enabled, "kill_switch_enabled");
    expect_equal(
        policy.min_post_risk_total_edge_tick,
        0LL,
        "min_post_risk_total_edge_tick"
    );
    expect_equal(
        policy.min_post_risk_unit_edge_tick,
        0LL,
        "min_post_risk_unit_edge_tick"
    );
    expect_equal(policy.min_edge_bps, 0LL, "min_edge_bps");
    expect_equal(policy.max_book_age_ns, 1'000'000'000LL, "max_book_age_ns");
    expect_equal(
        policy.max_intent_age_ns,
        1'000'000'000LL,
        "max_intent_age_ns"
    );
    expect_equal(policy.max_pending_intents_per_bundle, 1U, "pending bundle");
    expect_equal(policy.max_pending_intents_total, 1024U, "pending total");
    expect_equal(policy.max_approvals_per_second, 100U, "approvals");
}

void RiskDecision_DefaultReject() {
    const RiskDecision decision;

    expect_equal(
        decision.status,
        RiskDecisionStatus::Rejected,
        "status"
    );
    expect_equal(
        decision.reject_reason,
        RiskRejectReason::NotEvaluated,
        "reject_reason"
    );
    expect_false(decision.approved(), "approved");
    expect_true(decision.rejected(), "rejected");
}

void ApprovedIntent_RequiresReservationId() {
    ApprovedIntent approved;
    approved.decision = make_approved_decision(1, 123);

    expect_true(approved.decision.approved(), "decision approved");
    expect_false(approved.has_reservation(), "default reservation");
    expect_false(approved.valid(), "approved without reservation");

    approved.reservation_id = "reservation-1";
    expect_true(approved.has_reservation(), "reservation present");
    expect_true(approved.valid(), "approved with reservation");
}

void RiskPolicySnapshot_HashDeterministic() {
    RiskPolicySnapshot policy_a;
    RiskPolicySnapshot policy_b;

    const auto hash_a = compute_policy_hash(policy_a);
    const auto hash_b = compute_policy_hash(policy_b);
    expect_true(hash_a != 0, "hash nonzero");
    expect_equal(hash_a, hash_b, "stable hash");

    policy_b.max_total_cost_tick = 100;
    expect_true(
        compute_policy_hash(policy_b) != hash_a,
        "changed policy changes hash"
    );

    const auto with_hash = with_computed_policy_hash(policy_a);
    expect_equal(with_hash.policy_hash, hash_a, "with hash");
}

void RiskConfig_DefaultsAreSafe() {
    const RiskConfig config;

    expect_true(config.enabled, "enabled");
    expect_true(config.emit_rejections, "emit_rejections");
    expect_equal(config.max_decisions_per_scan, 1024U, "max decisions");
}

void RiskResult_DefaultCountersZero() {
    const RiskResult result;

    expect_equal(result.intents_evaluated, 0ULL, "intents_evaluated");
    expect_equal(result.intents_approved, 0ULL, "intents_approved");
    expect_equal(result.intents_rejected, 0ULL, "intents_rejected");
    expect_equal(result.output_hash, 0ULL, "output_hash");
    expect_equal(result.metrics.evaluate_count, 0ULL, "metrics evaluate");
    expect_equal(result.metrics.reject_count, 0ULL, "metrics reject");
}

void RiskAuditTrace_DefaultsAreSafe() {
    const RiskAuditTrace trace;

    expect_equal(trace.trace_id, 0ULL, "trace_id");
    expect_equal(trace.intent_id, 0ULL, "intent_id");
    expect_equal(trace.bundle_id, 0ULL, "bundle_id");
    expect_equal(trace.policy_version, 0ULL, "policy_version");
    expect_true(trace.decision.rejected(), "default decision rejected");
    expect_true(trace.evidence.empty(), "evidence");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "RiskPolicySnapshot_DefaultsAreSafe",
            &RiskPolicySnapshot_DefaultsAreSafe
        },
        {"RiskDecision_DefaultReject", &RiskDecision_DefaultReject},
        {
            "ApprovedIntent_RequiresReservationId",
            &ApprovedIntent_RequiresReservationId
        },
        {
            "RiskPolicySnapshot_HashDeterministic",
            &RiskPolicySnapshot_HashDeterministic
        },
        {"RiskConfig_DefaultsAreSafe", &RiskConfig_DefaultsAreSafe},
        {"RiskResult_DefaultCountersZero", &RiskResult_DefaultCountersZero},
        {"RiskAuditTrace_DefaultsAreSafe", &RiskAuditTrace_DefaultsAreSafe}
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
