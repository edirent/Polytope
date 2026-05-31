#include "engine/risk/core/RiskContext.h"
#include "engine/risk/publish/HashOnlyRiskPublisher.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::risk::HashOnlyRiskPublisher;
using trading_engine::risk::RiskDecisionStatus;
using trading_engine::risk::RiskPipelineResult;
using trading_engine::risk::RiskRejectReason;
using trading_engine::signal::IntentStatus;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
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

RiskPipelineResult approved_result() {
    RiskPipelineResult result;
    result.decision.status = RiskDecisionStatus::Approved;
    result.decision.reject_reason = RiskRejectReason::None;
    result.decision.decision_id = 11;
    result.decision.intent_id = 22;
    result.decision.bundle_id = 33;
    result.decision.policy_hash = 44;
    result.cost.ok = true;
    result.cost.risk_bundle_qty = 2;
    result.cost.risk_total_cost_tick = 1'500;
    result.cost.fee_tick = 10;
    result.cost.slippage_buffer_tick = 20;
    result.cost.latency_buffer_tick = 30;
    result.reservation.reservation_id = 55;
    result.approved_intent.intent.intent_id = 22;
    result.approved_intent.intent.bundle_id = 33;
    result.approved_intent.intent.status = IntentStatus::PaperOpportunity;
    result.approved_intent.intent.guaranteed_payout_tick = 1'000;
    return result;
}

void HashOnlyRiskPublisher_PublishesNumericRecord() {
    HashOnlyRiskPublisher publisher;
    publisher.publish_result(approved_result());

    expect_equal(publisher.records().size(), 1U, "record count");
    const auto& record = publisher.records().front();
    expect_equal(record.decision_id, 11ULL, "decision id");
    expect_equal(record.intent_id, 22ULL, "intent id");
    expect_equal(record.bundle_id, 33ULL, "bundle id");
    expect_equal(record.decision, RiskDecisionStatus::Approved, "status");
    expect_equal(record.policy_hash, 44ULL, "policy hash");
    expect_equal(record.reservation_id, 55ULL, "reservation");
    expect_equal(record.risk_total_edge_tick, 440LL, "risk edge");
}

void HashOnlyRiskPublisher_DoesNotMaterializeStrings() {
    HashOnlyRiskPublisher publisher;
    auto result = approved_result();
    result.decision.reject_detail = "not-used";
    result.audit_trace.steps.push_back({
        "ExpensiveGuardName",
        true,
        trading_engine::risk::RiskDecisionType::Pass,
        "expensive reason"
    });

    publisher.publish_result(result);

    expect_equal(publisher.records().size(), 1U, "record count");
    expect_equal(publisher.records().front().intent_id, 22ULL, "intent");
}

void HashOnlyRiskPublisher_PreservesIntentIdBundleId() {
    HashOnlyRiskPublisher publisher;
    auto first = approved_result();
    auto second = approved_result();
    second.decision.intent_id = 23;
    second.decision.bundle_id = 34;
    publisher.publish_result(first);
    publisher.publish_result(second);

    expect_equal(publisher.records()[0].intent_id, 22ULL, "first intent");
    expect_equal(publisher.records()[1].intent_id, 23ULL, "second intent");
    expect_equal(publisher.records()[1].bundle_id, 34ULL, "second bundle");
}

void HashOnlyRiskPublisher_DeterministicOutputHash() {
    HashOnlyRiskPublisher left;
    HashOnlyRiskPublisher right;
    left.publish_result(approved_result());
    right.publish_result(approved_result());

    expect_true(left.output_hash() != 0, "hash nonzero");
    expect_equal(left.output_hash(), right.output_hash(), "hash");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "HashOnlyRiskPublisher_PublishesNumericRecord",
            &HashOnlyRiskPublisher_PublishesNumericRecord
        },
        {
            "HashOnlyRiskPublisher_DoesNotMaterializeStrings",
            &HashOnlyRiskPublisher_DoesNotMaterializeStrings
        },
        {
            "HashOnlyRiskPublisher_PreservesIntentIdBundleId",
            &HashOnlyRiskPublisher_PreservesIntentIdBundleId
        },
        {
            "HashOnlyRiskPublisher_DeterministicOutputHash",
            &HashOnlyRiskPublisher_DeterministicOutputHash
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
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " TEST_NAME\n";
        return 2;
    }
    return run_test(argv[1]);
}
