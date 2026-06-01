#include "engine/execution/public/ExecutionReport.h"
#include "engine/execution/public/OrderPlan.h"
#include "engine/paper/ledger/PaperEventAdapter.h"
#include "engine/paper/ledger/PaperLedger.h"
#include "engine/risk/public/RiskDecision.h"
#include "engine/signal/public/OpportunityIntent.h"

#include <iostream>
#include <string>
#include <type_traits>

namespace {

using trading_engine::execution::ChildOrderStatus;
using trading_engine::execution::ExecutionReport;
using trading_engine::execution::OrderPlan;
using trading_engine::execution::OrderSide;
using trading_engine::paper::PaperEventAdapter;
using trading_engine::paper::PaperEventType;
using trading_engine::paper::PaperLedger;
using trading_engine::risk::RiskDecision;
using trading_engine::risk::RiskDecisionStatus;
using trading_engine::risk::RiskRejectReason;
using trading_engine::signal::OpportunityIntent;

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

template <typename T, typename U>
int expect_equal(const T& actual, const U& expected, const char* message) {
    if (!(actual == expected)) {
        std::cerr << message << ": expected ";
        if constexpr (std::is_enum_v<U>) {
            std::cerr << static_cast<int>(expected);
        } else {
            std::cerr << expected;
        }
        std::cerr << ", got ";
        if constexpr (std::is_enum_v<T>) {
            std::cerr << static_cast<int>(actual);
        } else {
            std::cerr << actual;
        }
        std::cerr << '\n';
        return 1;
    }
    return 0;
}

OrderPlan plan_with_buy_order() {
    OrderPlan plan;
    plan.plan_id = 42;
    plan.created_ts_ns = 100;
    plan.order_count = 1;
    plan.orders[0].order_id = 7;
    plan.orders[0].plan_id = plan.plan_id;
    plan.orders[0].market_id = "market";
    plan.orders[0].asset_id = "asset";
    plan.orders[0].side = OrderSide::Buy;
    plan.orders[0].quantity_lots = 10;
    plan.orders[0].limit_price_tick = 100;
    return plan;
}

ExecutionReport filled_report() {
    ExecutionReport report;
    report.plan_id = 42;
    report.child_order_id = 7;
    report.status = ChildOrderStatus::Filled;
    report.filled_lots = 10;
    report.remaining_lots = 0;
    report.avg_fill_price_tick = 100;
    report.event_ts_ns = 200;
    return report;
}

int test_execution_report_creates_fill_event() {
    PaperEventAdapter adapter;
    (void)adapter.observe(plan_with_buy_order());

    const auto result = adapter.observe(filled_report());
    if (const auto check = expect_equal(
            result.event.type,
            PaperEventType::ExecutionReportObserved,
            "event type"
        );
        check != 0) {
        return check;
    }
    if (!result.has_fill) {
        return fail("execution report should create fill application");
    }
    if (const auto check =
            expect_equal(result.fill.asset_id, std::string{"asset"}, "asset id");
        check != 0) {
        return check;
    }
    if (const auto check =
            expect_equal(result.fill.report.filled_lots, 10LL, "filled lots");
        check != 0) {
        return check;
    }

    PaperLedger ledger{10'000};
    const auto applied = ledger.apply_fill(result.fill);
    if (!applied.applied) {
        return fail("adapter fill should be ledger-applicable");
    }
    return expect_equal(
        applied.snapshot.cash.cash_tick,
        9'000LL,
        "cash after fill"
    );
}

int test_opportunity_intent_does_not_create_fill() {
    PaperEventAdapter adapter;
    OpportunityIntent intent;
    intent.intent_id = 99;
    intent.created_ts_ns = 123;

    const auto result = adapter.observe(intent);
    if (const auto check = expect_equal(
            result.event.type,
            PaperEventType::OpportunityIntentObserved,
            "event type"
        );
        check != 0) {
        return check;
    }
    if (result.has_fill) {
        return fail("opportunity intent must not create fill application");
    }
    return expect_equal(result.event.ts_ns, 123ULL, "event timestamp");
}

int test_risk_decision_does_not_change_ledger() {
    PaperEventAdapter adapter;
    PaperLedger ledger{10'000};
    const auto before = ledger.snapshot();

    RiskDecision decision;
    decision.status = RiskDecisionStatus::Rejected;
    decision.reject_reason = RiskRejectReason::TotalExposureLimit;

    const auto result = adapter.observe(decision);
    if (const auto check = expect_equal(
            result.event.type,
            PaperEventType::RiskDecisionObserved,
            "event type"
        );
        check != 0) {
        return check;
    }
    if (result.has_fill) {
        return fail("risk decision must not create fill application");
    }

    const auto after = ledger.snapshot();
    if (const auto check =
            expect_equal(after.cash.cash_tick, before.cash.cash_tick, "cash");
        check != 0) {
        return check;
    }
    return expect_equal(
        after.position_count,
        before.position_count,
        "position count"
    );
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return fail("expected one test case name");
    }

    const std::string test_case{argv[1]};
    if (test_case == "Adapter_ExecutionReportCreatesFillEvent") {
        return test_execution_report_creates_fill_event();
    }
    if (test_case == "Adapter_OpportunityIntentDoesNotCreateFill") {
        return test_opportunity_intent_does_not_create_fill();
    }
    if (test_case == "Adapter_RiskDecisionDoesNotChangeLedger") {
        return test_risk_decision_does_not_change_ledger();
    }

    return fail("unknown test case");
}
