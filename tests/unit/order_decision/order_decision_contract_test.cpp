#include "engine/order_decision/public/ApprovedOrderDecisionEnvelope.h"
#include "engine/order_decision/public/OrderDecision.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::order_decision::OrderDecision;
using trading_engine::order_decision::OrderDecisionLeg;
using trading_engine::order_decision::OrderDecisionType;
using trading_engine::order_decision::compute_approved_intent_hash;
using trading_engine::order_decision::make_approved_order_decision_envelope;
using trading_engine::oracle::Side;
using trading_engine::risk::make_approved_decision;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
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

void OrderDecision_DefaultNoTrade() {
    const OrderDecision decision;
    expect_equal(decision.type, OrderDecisionType::NoTrade, "type");
    expect_equal(decision.chosen_bundle_qty, 0LL, "qty");
    expect_equal(decision.leg_count, static_cast<std::uint16_t>(0), "legs");
}

void OrderDecision_HasSnapshotBundlePolicyHashes() {
    OrderDecision decision;
    decision.snapshot_version_hash = 11;
    decision.oracle_artifact_hash = 22;
    decision.bundle_hash = 33;
    decision.policy_hash = 44;
    expect_equal(decision.snapshot_version_hash, 11ULL, "snapshot");
    expect_equal(decision.oracle_artifact_hash, 22ULL, "artifact");
    expect_equal(decision.bundle_hash, 33ULL, "bundle");
    expect_equal(decision.policy_hash, 44ULL, "policy");
}

void OrderDecisionLeg_HasQtyAndLimitPrice() {
    OrderDecisionLeg leg;
    leg.side = Side::Buy;
    leg.quantity_lots = 7;
    leg.limit_price_tick = 510'000;
    leg.worst_price_tick = 500'000;
    expect_equal(leg.side, Side::Buy, "side");
    expect_equal(leg.quantity_lots, 7LL, "qty");
    expect_equal(leg.limit_price_tick, 510'000LL, "limit");
}

void ApprovedOrderDecisionEnvelope_HasDecisionAndApprovalHashes() {
    OrderDecision decision;
    decision.source_intent_id = 101;
    decision.bundle_id = 202;
    decision.type = OrderDecisionType::PaperOrderDecision;
    decision.chosen_bundle_qty = 7;
    decision.expires_at_ns = 3000;
    decision.decision_hash =
        trading_engine::order_decision::compute_order_decision_hash(decision);

    trading_engine::risk::ApprovedIntent approved;
    approved.intent.intent_id = decision.source_intent_id;
    approved.intent.bundle_id = decision.bundle_id;
    approved.intent.idempotency_hash = 303;
    approved.decision = make_approved_decision(1, 2);
    approved.decision.decision_id = 404;
    approved.decision.intent_id = decision.source_intent_id;
    approved.decision.bundle_id = decision.bundle_id;
    approved.reservation_id_hash = 505;
    approved.approved_at_ns = 1200;
    approved.expires_at_ns = 3000;

    const auto expected_approval_hash = compute_approved_intent_hash(approved);
    const auto envelope = make_approved_order_decision_envelope(
        approved,
        decision,
        1300
    );

    expect_equal(envelope.source_intent_id, 101ULL, "source_intent_id");
    expect_equal(envelope.bundle_id, 202ULL, "bundle_id");
    expect_equal(
        envelope.decision_hash,
        decision.decision_hash,
        "decision_hash"
    );
    expect_equal(envelope.approval_hash, expected_approval_hash, "approval_hash");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"OrderDecision_DefaultNoTrade", &OrderDecision_DefaultNoTrade},
        {
            "OrderDecision_HasSnapshotBundlePolicyHashes",
            &OrderDecision_HasSnapshotBundlePolicyHashes
        },
        {
            "OrderDecisionLeg_HasQtyAndLimitPrice",
            &OrderDecisionLeg_HasQtyAndLimitPrice
        },
        {
            "ApprovedOrderDecisionEnvelope_HasDecisionAndApprovalHashes",
            &ApprovedOrderDecisionEnvelope_HasDecisionAndApprovalHashes
        }
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
