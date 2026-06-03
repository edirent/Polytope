#include "engine/execution/public/ExecutionReport.h"
#include "engine/execution/public/MakerExecutionTypes.h"
#include "engine/execution/public/OrderPlan.h"
#include "engine/paper/ledger/PaperEventAdapter.h"
#include "engine/paper/ledger/PaperLedger.h"
#include "engine/risk/public/ApprovedQuote.h"
#include "engine/risk/public/QuoteRiskDecision.h"
#include "engine/risk/public/RiskDecision.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "engine/strategy/market_making/public/QuoteIntent.h"

#include <iostream>
#include <string>
#include <type_traits>

namespace {

using trading_engine::execution::ChildOrderStatus;
using trading_engine::execution::ExecutionReport;
using trading_engine::execution::FillLiquidityRole;
using trading_engine::execution::MakerExecutionReport;
using trading_engine::execution::MakerQuoteStatus;
using trading_engine::execution::OrderPlan;
using trading_engine::execution::OrderSide;
using trading_engine::execution::QuoteSide;
using trading_engine::paper::PaperEventAdapter;
using trading_engine::paper::PaperEventType;
using trading_engine::paper::PaperLedger;
using trading_engine::risk::ApprovedQuote;
using trading_engine::risk::QuoteRiskDecision;
using trading_engine::risk::RiskDecision;
using trading_engine::risk::RiskDecisionStatus;
using trading_engine::risk::RiskRejectReason;
using trading_engine::signal::OpportunityIntent;
using trading_engine::strategy::market_making::QuoteIntent;

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

MakerExecutionReport maker_report(std::int64_t filled_qty_lots) {
    MakerExecutionReport report;
    report.report_id = 901;
    report.quote_id = 902;
    report.approved_quote_id = 903;
    report.quote_group_id = 904;
    report.asset_index = 7;
    report.asset_id = "asset_yes";
    report.side = QuoteSide::Bid;
    report.status = filled_qty_lots > 0 ? MakerQuoteStatus::Filled
                                         : MakerQuoteStatus::ActivePaper;
    report.liquidity_role = FillLiquidityRole::Maker;
    report.filled_qty_lots = filled_qty_lots;
    report.avg_fill_price_tick = 490'000;
    report.recv_ts_ns = 1'000;
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

int test_quote_intent_does_not_create_fill() {
    PaperEventAdapter adapter;
    QuoteIntent intent;
    intent.quote_intent_id = 1;
    intent.created_ts_ns = 2;

    const auto result = adapter.observe(intent);
    if (const auto check = expect_equal(
            result.event.type,
            PaperEventType::QuoteIntentObserved,
            "event type"
        );
        check != 0) {
        return check;
    }
    if (result.has_fill || result.has_paper_fill) {
        return fail("quote intent must not create fill");
    }
    return 0;
}

int test_approved_quote_does_not_create_fill() {
    PaperEventAdapter adapter;
    ApprovedQuote quote;
    quote.approved_quote_id = 1;
    quote.approved_ts_ns = 2;

    const auto result = adapter.observe(quote);
    if (const auto check = expect_equal(
            result.event.type,
            PaperEventType::ApprovedQuoteObserved,
            "event type"
        );
        check != 0) {
        return check;
    }
    if (result.has_fill || result.has_paper_fill) {
        return fail("approved quote must not create fill");
    }
    return 0;
}

int test_quote_risk_decision_does_not_create_fill() {
    PaperEventAdapter adapter;
    QuoteRiskDecision decision;
    decision.decision_ts_ns = 2;

    const auto result = adapter.observe(decision);
    if (const auto check = expect_equal(
            result.event.type,
            PaperEventType::QuoteRiskDecisionObserved,
            "event type"
        );
        check != 0) {
        return check;
    }
    if (result.has_fill || result.has_paper_fill) {
        return fail("quote risk decision must not create fill");
    }
    return 0;
}

int test_maker_execution_report_with_fill_creates_paper_fill() {
    PaperEventAdapter adapter;
    const auto result = adapter.observe(maker_report(100));
    if (const auto check = expect_equal(
            result.event.type,
            PaperEventType::ExecutionReportObserved,
            "event type"
        );
        check != 0) {
        return check;
    }
    if (!result.has_paper_fill) {
        return fail("maker fill report should create paper fill");
    }
    return expect_equal(result.paper_fill.qty_lots, 100LL, "paper fill qty");
}

int test_maker_execution_report_without_fill_does_not_create_paper_fill() {
    PaperEventAdapter adapter;
    const auto result = adapter.observe(maker_report(0));
    if (result.has_fill || result.has_paper_fill) {
        return fail("unfilled maker report must not create fill");
    }
    return 0;
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
    if (test_case == "Adapter_QuoteIntentDoesNotCreateFill") {
        return test_quote_intent_does_not_create_fill();
    }
    if (test_case == "Adapter_ApprovedQuoteDoesNotCreateFill") {
        return test_approved_quote_does_not_create_fill();
    }
    if (test_case == "Adapter_QuoteRiskDecisionDoesNotCreateFill") {
        return test_quote_risk_decision_does_not_create_fill();
    }
    if (test_case == "Adapter_MakerExecutionReportWithFillCreatesPaperFill") {
        return test_maker_execution_report_with_fill_creates_paper_fill();
    }
    if (test_case == "Adapter_MakerExecutionReportWithoutFillDoesNotCreatePaperFill") {
        return test_maker_execution_report_without_fill_does_not_create_paper_fill();
    }

    return fail("unknown test case");
}
