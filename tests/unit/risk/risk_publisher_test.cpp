#include "engine/risk/core/RiskEngine.h"
#include "engine/risk/publish/ApprovedIntentPublisher.h"
#include "engine/risk/publish/CapturingRiskPublisher.h"
#include "engine/risk/publish/JsonlRiskDecisionWriter.h"

#include <boost/json.hpp>

#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using trading_engine::risk::CapturingApprovedIntentPublisher;
using trading_engine::risk::CapturingRiskPublisher;
using trading_engine::risk::JsonlRiskDecisionWriter;
using trading_engine::risk::RiskDecisionType;
using trading_engine::risk::RiskEngine;
using trading_engine::risk::RiskEvaluationContext;
using trading_engine::risk::RiskRejectReason;
using trading_engine::risk::RiskAuditTrace;
using trading_engine::signal::IntentStatus;
using trading_engine::signal::OpportunityIntent;
using trading_engine::state::MarketStateSnapshot;

namespace json = boost::json;

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

MarketStateSnapshot snapshot(std::string asset_id = "asset-1") {
    MarketStateSnapshot out;
    out.entity_id = std::move(asset_id);
    out.market_id = "market";
    out.version = 7;
    out.last_book_update_ns = 1'400;
    out.live = true;
    out.has_ask = true;
    out.ask_count = 1;
    out.asks[0].price_tick = 800;
    out.asks[0].size = 100.0;
    out.usable_for_depth = true;
    out.state_hash = 111;
    return out;
}

OpportunityIntent intent(std::string key = "risk-publish") {
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
    out.bundle_hash = 456;
    out.snapshot_version = 7;
    out.snapshot_version_hash = 111;
    out.bundle_qty = 10;
    out.unit_edge_tick = 1'200;
    out.total_edge_tick = 12'000;
    out.created_ts_ns = 1'000;
    out.expires_at_ns = 2'000;
    out.idempotency_key = std::move(key);
    out.leg_count = 1;
    out.legs[0].market_id = "market";
    out.legs[0].asset_id = "asset-1";
    out.legs[0].quantity_lots = 1;
    out.legs[0].estimated_cost_tick = 8'000;
    return out;
}

RiskEvaluationContext context() {
    RiskEvaluationContext out;
    out.now_ns = 1'500;
    out.latest_snapshots = {snapshot()};
    return out;
}

void RiskAuditTrace_RecordsEveryExecutedGuard() {
    RiskEngine engine;
    const auto result = engine.evaluate(intent(), context());

    expect_true(result.decision.approved(), "approved");
    const auto& steps = result.audit_trace.steps;
    expect_true(steps.size() >= 15U, "step count");
    expect_equal(steps.front().guard_name, std::string{"KillSwitchGuard"}, "first");
    expect_equal(
        steps.back().guard_name,
        std::string{"ReservationBook.try_reserve"},
        "last"
    );
    for (const auto& step : steps) {
        expect_true(step.pass, step.guard_name);
    }
}

void RiskAuditTrace_StopsAtRejectingGuard() {
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
    const auto& steps = result.audit_trace.steps;
    expect_equal(
        steps.back().guard_name,
        std::string{"IntentValidator"},
        "rejecting step"
    );
    expect_false(steps.back().pass, "rejecting step pass");
}

void CapturingRiskPublisher_CapturesDecisionAndTrace() {
    RiskEngine engine;
    const auto result = engine.evaluate(intent(), context());

    CapturingRiskPublisher publisher;
    publisher.publish_decision(result.decision, result.audit_trace);

    expect_equal(publisher.decisions().size(), 1U, "decisions");
    expect_equal(
        publisher.decisions()[0].trace.intent_id,
        100ULL,
        "trace intent"
    );
    expect_true(
        publisher.decisions()[0].decision.approved(),
        "captured approved"
    );
}

void CapturingApprovedIntentPublisher_CapturesApprovedIntent() {
    RiskEngine engine;
    const auto result = engine.evaluate(intent(), context());

    CapturingApprovedIntentPublisher publisher;
    publisher.publish_approved(result.approved_intent);

    expect_equal(publisher.approved_intents().size(), 1U, "approved");
    expect_true(publisher.approved_intents()[0].valid(), "valid");
}

void JsonlRiskDecisionWriter_WritesAuditTrace() {
    RiskEngine engine;
    const auto result = engine.evaluate(intent(), context());

    std::ostringstream output;
    JsonlRiskDecisionWriter writer(&output);
    expect_true(writer.write(result.decision, result.audit_trace), "write");

    const auto parsed = json::parse(output.str());
    expect_true(parsed.is_object(), "json object");
    const auto& object = parsed.as_object();
    expect_equal(
        json::value_to<std::string>(object.at("status")),
        std::string{"Approved"},
        "status"
    );
    expect_true(object.at("steps").is_array(), "steps");
    expect_true(!object.contains("order"), "no order field");
    expect_true(output.str().find("order") == std::string::npos, "no order text");
}

void JsonlRiskDecisionWriter_DoesNotContainOrderFields() {
    RiskAuditTrace trace;
    trace.trace_id = 1;
    trace.intent_id = 2;
    trace.bundle_id = 3;
    trace.steps.push_back({
        "KillSwitchGuard",
        true,
        RiskDecisionType::Pass,
        {}
    });

    std::ostringstream output;
    JsonlRiskDecisionWriter writer(&output);
    expect_true(writer.write(trace.decision, trace), "write");

    const auto parsed = json::parse(output.str());
    expect_true(parsed.is_object(), "json object");
    expect_true(!parsed.as_object().contains("order_id"), "no order_id");
    expect_true(output.str().find("order") == std::string::npos, "no order");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"RiskAuditTrace_RecordsEveryExecutedGuard", &RiskAuditTrace_RecordsEveryExecutedGuard},
        {"RiskAuditTrace_StopsAtRejectingGuard", &RiskAuditTrace_StopsAtRejectingGuard},
        {"CapturingRiskPublisher_CapturesDecisionAndTrace", &CapturingRiskPublisher_CapturesDecisionAndTrace},
        {"CapturingApprovedIntentPublisher_CapturesApprovedIntent", &CapturingApprovedIntentPublisher_CapturesApprovedIntent},
        {"JsonlRiskDecisionWriter_WritesAuditTrace", &JsonlRiskDecisionWriter_WritesAuditTrace},
        {"JsonlRiskDecisionWriter_DoesNotContainOrderFields", &JsonlRiskDecisionWriter_DoesNotContainOrderFields}
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
