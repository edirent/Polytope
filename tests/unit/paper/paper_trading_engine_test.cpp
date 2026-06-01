#include "engine/execution/public/ExecutionReport.h"
#include "engine/execution/public/OrderPlan.h"
#include "engine/paper/core/PaperTradingEngine.h"
#include "engine/paper/core/PaperTradingWorkflow.h"
#include "engine/risk/public/RiskDecision.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "engine/state/MarketStateSnapshot.h"

#include <iostream>
#include <string>

namespace {

using trading_engine::execution::ChildOrderStatus;
using trading_engine::execution::ExecutionReport;
using trading_engine::execution::OrderPlan;
using trading_engine::execution::OrderSide;
using trading_engine::paper::PaperTradingEngine;
using trading_engine::paper::PaperTradingWorkflow;
using trading_engine::risk::RiskDecision;
using trading_engine::risk::RiskDecisionStatus;
using trading_engine::risk::RiskRejectReason;
using trading_engine::signal::IntentStatus;
using trading_engine::signal::OpportunityIntent;
using trading_engine::state::MarketStateSnapshot;
using trading_engine::state::PriceLevel;

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

template <typename T, typename U>
int expect_equal(const T& actual, const U& expected, const char* message) {
    if (!(actual == expected)) {
        std::cerr << message << ": expected " << expected << ", got " << actual
                  << '\n';
        return 1;
    }
    return 0;
}

OrderPlan plan() {
    OrderPlan out;
    out.plan_id = 77;
    out.source_intent_id = 44;
    out.approved_intent_id = 55;
    out.reservation_id = 66;
    out.bundle_id = 88;
    out.order_count = 1;
    out.chosen_bundle_qty = 10;
    out.guaranteed_payout_tick = 10'000;
    out.expected_terminal_pnl_tick = 3'000;
    out.created_ts_ns = 1000;
    out.expire_after_ns = 2000;
    out.idempotency_key = "idem";
    out.orders[0].order_id = 11;
    out.orders[0].plan_id = out.plan_id;
    out.orders[0].market_id = "m1";
    out.orders[0].asset_id = "asset_yes";
    out.orders[0].asset_index = 7;
    out.orders[0].side = OrderSide::Buy;
    out.orders[0].quantity_lots = 10;
    out.orders[0].limit_price_tick = 800;
    return out;
}

ExecutionReport filled_report() {
    ExecutionReport report;
    report.plan_id = 77;
    report.child_order_id = 11;
    report.status = ChildOrderStatus::Filled;
    report.filled_lots = 10;
    report.remaining_lots = 0;
    report.avg_fill_price_tick = 700;
    report.event_ts_ns = 1100;
    return report;
}

MarketStateSnapshot mark_snapshot() {
    MarketStateSnapshot snapshot;
    snapshot.entity_id = "asset_yes";
    snapshot.market_id = "m1";
    snapshot.version = 3;
    snapshot.last_book_update_ns = 1200;
    snapshot.usable_for_depth = true;
    snapshot.bid_count = 1;
    snapshot.ask_count = 1;
    snapshot.bids[0] = PriceLevel{800, 0.0, 100.0};
    snapshot.asks[0] = PriceLevel{1000, 0.0, 100.0};
    return snapshot;
}

int test_consumes_execution_report_and_updates_dashboard() {
    PaperTradingEngine engine{100'000};
    engine.on_order_plan(plan());
    engine.on_execution_report(filled_report());
    engine.on_mark_snapshot(mark_snapshot());

    const auto latest = engine.latest_dashboard_snapshot();
    if (const auto check = expect_equal(
            latest.account.cash_balance_tick,
            93'000LL,
            "cash after fill"
        );
        check != 0) {
        return check;
    }
    if (const auto check = expect_equal(
            latest.account.unrealized_pnl_tick,
            2'000LL,
            "unrealized pnl after mark"
        );
        check != 0) {
        return check;
    }
    if (const auto check = expect_equal(
            latest.execution.plans_created,
            1ULL,
            "plans created"
        );
        check != 0) {
        return check;
    }
    if (const auto check =
            expect_equal(latest.execution.plans_filled, 1ULL, "plans filled");
        check != 0) {
        return check;
    }
    if (const auto check = expect_equal(
            latest.filled_orders.size(),
            1ULL,
            "filled order rows"
        );
        check != 0) {
        return check;
    }
    if (const auto check = expect_equal(
            latest.filled_orders[0].asset_id,
            std::string{"asset_yes"},
            "filled order asset"
        );
        check != 0) {
        return check;
    }
    if (const auto check = expect_equal(
            latest.filled_orders[0].asset_index,
            7U,
            "filled order asset index"
        );
        check != 0) {
        return check;
    }
    if (const auto check = expect_equal(
            latest.filled_orders[0].notional_tick,
            7'000LL,
            "filled order notional"
        );
        check != 0) {
        return check;
    }
    if (const auto check = expect_equal(
            latest.filled_orders[0].unrealized_pnl_tick,
            2'000LL,
            "filled order pnl"
        );
        check != 0) {
        return check;
    }
    if (const auto check = expect_equal(
            latest.terminal_pnl.size(),
            1ULL,
            "terminal pnl rows"
        );
        check != 0) {
        return check;
    }
    if (const auto check = expect_equal(
            latest.terminal_pnl[0].terminal_pnl_tick,
            3'000LL,
            "terminal pnl"
        );
        check != 0) {
        return check;
    }
    return expect_equal(
        latest.performance.terminal_pnl_tick,
        3'000LL,
        "performance terminal pnl"
    );
}

int test_opportunity_intent_observation_does_not_change_pnl() {
    PaperTradingEngine engine{100'000};
    OpportunityIntent intent;
    intent.intent_id = 1;
    intent.bundle_id = 2;
    intent.status = IntentStatus::PaperOpportunity;
    intent.created_ts_ns = 10;

    engine.on_opportunity_intent(intent);

    const auto latest = engine.latest_dashboard_snapshot();
    if (const auto check =
            expect_equal(latest.signal.intents_published, 1ULL, "intents");
        check != 0) {
        return check;
    }
    if (const auto check = expect_equal(
            latest.account.cash_balance_tick,
            100'000LL,
            "cash unchanged"
        );
        check != 0) {
        return check;
    }
    return expect_equal(
        latest.performance.gross_pnl_tick,
        0LL,
        "pnl unchanged"
    );
}

int test_risk_decision_observation_does_not_change_ledger() {
    PaperTradingEngine engine{100'000};
    RiskDecision decision;
    decision.status = RiskDecisionStatus::Rejected;
    decision.reject_reason = RiskRejectReason::LowTotalEdge;

    engine.on_risk_decision(decision);

    const auto latest = engine.latest_dashboard_snapshot();
    if (const auto check =
            expect_equal(latest.risk.decisions, 1ULL, "risk decisions");
        check != 0) {
        return check;
    }
    if (const auto check =
            expect_equal(latest.risk.rejected, 1ULL, "risk rejected");
        check != 0) {
        return check;
    }
    return expect_equal(
        latest.account.cash_balance_tick,
        100'000LL,
        "cash unchanged"
    );
}

int test_workflow_exposes_latest_dashboard() {
    PaperTradingWorkflow workflow{50'000};
    OpportunityIntent intent;
    intent.status = IntentStatus::RejectedLowEdge;
    intent.created_ts_ns = 17;
    workflow.engine().on_opportunity_intent(intent);

    const auto latest = workflow.latest_dashboard_snapshot();
    if (const auto check =
            expect_equal(latest.signal.rejected, 1ULL, "signal rejected");
        check != 0) {
        return check;
    }
    return expect_equal(
        latest.account.cash_balance_tick,
        50'000LL,
        "workflow cash"
    );
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return fail("expected one test case name");
    }

    const std::string test_case{argv[1]};
    if (test_case == "PaperTradingEngine_ExecutionReportUpdatesDashboard") {
        return test_consumes_execution_report_and_updates_dashboard();
    }
    if (test_case == "PaperTradingEngine_OpportunityDoesNotCreatePnL") {
        return test_opportunity_intent_observation_does_not_change_pnl();
    }
    if (test_case == "PaperTradingEngine_RiskDecisionDoesNotChangeLedger") {
        return test_risk_decision_observation_does_not_change_ledger();
    }
    if (test_case == "PaperTradingWorkflow_ExposesLatestDashboard") {
        return test_workflow_exposes_latest_dashboard();
    }

    return fail("unknown test case");
}
