#include "engine/paper/ledger/PaperLedger.h"
#include "engine/paper/public/PaperEvent.h"

#include <iostream>
#include <string>

namespace {

using trading_engine::execution::ChildOrderStatus;
using trading_engine::execution::OrderSide;
using trading_engine::paper::FillApplication;
using trading_engine::paper::PaperEvent;
using trading_engine::paper::PaperEventType;
using trading_engine::paper::PaperLedger;
using trading_engine::paper::PaperLedgerApplyStatus;

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

FillApplication buy_fill(
    std::uint64_t report_id,
    std::int64_t lots,
    std::int64_t price_tick,
    std::int64_t fee_tick = 0
) {
    FillApplication fill;
    fill.execution_report_id = report_id;
    fill.asset_id = "asset_yes";
    fill.market_id = "market_1";
    fill.side = OrderSide::Buy;
    fill.fee_tick = fee_tick;
    fill.report.plan_id = 10;
    fill.report.child_order_id = 20;
    fill.report.status = ChildOrderStatus::Filled;
    fill.report.filled_lots = lots;
    fill.report.remaining_lots = 0;
    fill.report.avg_fill_price_tick = price_tick;
    fill.report.event_ts_ns = 123;
    return fill;
}

int test_initial_cash() {
    PaperLedger ledger{1'000'000};
    const auto snapshot = ledger.snapshot();
    if (const auto result =
            expect_equal(snapshot.cash.starting_cash_tick, 1'000'000LL, "starting cash");
        result != 0) {
        return result;
    }
    if (const auto result =
            expect_equal(snapshot.cash.cash_tick, 1'000'000LL, "cash");
        result != 0) {
        return result;
    }
    if (const auto result =
            expect_equal(snapshot.applied_execution_report_count, 0U, "applied reports");
        result != 0) {
        return result;
    }
    return 0;
}

int test_apply_buy_fill() {
    PaperLedger ledger{1'000'000};
    const auto result = ledger.apply_fill(buy_fill(1, 10, 700));
    if (!result.applied || result.status != PaperLedgerApplyStatus::Applied) {
        return fail("buy fill should apply");
    }
    if (const auto check =
            expect_equal(ledger.position_ledger().lots("asset_yes"), 10LL, "position lots");
        check != 0) {
        return check;
    }
    if (const auto check = expect_equal(
            ledger.position_ledger().cost_basis_tick("asset_yes"),
            7000LL,
            "position cost basis"
        );
        check != 0) {
        return check;
    }
    return 0;
}

int test_rejects_duplicate_execution_report() {
    PaperLedger ledger{1'000'000};
    const auto fill = buy_fill(99, 10, 700, 3);
    const auto first = ledger.apply_fill(fill);
    const auto after_first = ledger.snapshot();
    const auto second = ledger.apply_fill(fill);
    const auto after_second = ledger.snapshot();

    if (!first.applied) {
        return fail("first fill should apply");
    }
    if (second.applied ||
        second.status != PaperLedgerApplyStatus::DuplicateExecutionReport) {
        return fail("duplicate fill should be rejected as duplicate");
    }
    if (after_first.cash.cash_tick != after_second.cash.cash_tick ||
        after_first.cash.fees_paid_tick != after_second.cash.fees_paid_tick ||
        after_first.applied_execution_report_count !=
            after_second.applied_execution_report_count ||
        ledger.position_ledger().lots("asset_yes") != 10) {
        return fail("duplicate fill changed ledger state");
    }
    return 0;
}

int test_tracks_fees() {
    PaperLedger ledger{1'000'000};
    (void)ledger.apply_fill(buy_fill(1, 10, 700, 5));
    (void)ledger.apply_fill(buy_fill(2, 5, 600, 7));
    const auto snapshot = ledger.snapshot();
    if (const auto result =
            expect_equal(snapshot.cash.fees_paid_tick, 12LL, "fees paid");
        result != 0) {
        return result;
    }
    return 0;
}

int test_updates_cash() {
    PaperLedger ledger{1'000'000};
    (void)ledger.apply_fill(buy_fill(1, 10, 700, 5));
    const auto snapshot = ledger.snapshot();
    if (const auto result =
            expect_equal(snapshot.cash.cash_tick, 992'995LL, "cash after buy");
        result != 0) {
        return result;
    }
    return 0;
}

int test_observation_events_do_not_produce_pnl() {
    PaperLedger ledger{1'000'000};
    PaperEvent event;
    event.type = PaperEventType::OpportunityIntentObserved;
    event.seq_no = 1;
    event.ts_ns = 2;

    const auto before = ledger.snapshot();
    (void)event;
    const auto after = ledger.snapshot();

    if (before.cash.cash_tick != after.cash.cash_tick ||
        before.cash.realized_pnl_tick != after.cash.realized_pnl_tick ||
        before.cash.fees_paid_tick != after.cash.fees_paid_tick ||
        before.applied_execution_report_count !=
            after.applied_execution_report_count) {
        return fail("observation event changed ledger state");
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return fail("expected one test case name");
    }

    const std::string test_case{argv[1]};
    if (test_case == "PaperLedger_InitialCash") {
        return test_initial_cash();
    }
    if (test_case == "PaperLedger_ApplyBuyFill") {
        return test_apply_buy_fill();
    }
    if (test_case == "PaperLedger_RejectsDuplicateExecutionReport") {
        return test_rejects_duplicate_execution_report();
    }
    if (test_case == "PaperLedger_TracksFees") {
        return test_tracks_fees();
    }
    if (test_case == "PaperLedger_UpdatesCash") {
        return test_updates_cash();
    }
    if (test_case == "PaperLedger_OpportunityAndRiskEventsDoNotProducePnL") {
        return test_observation_events_do_not_produce_pnl();
    }

    return fail("unknown test case");
}
