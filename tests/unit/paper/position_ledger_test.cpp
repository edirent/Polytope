#include "engine/paper/ledger/PositionLedger.h"
#include "engine/paper/portfolio/PaperPortfolio.h"

#include <iostream>
#include <string>

namespace {

using trading_engine::execution::ChildOrderStatus;
using trading_engine::execution::OrderSide;
using trading_engine::paper::FillApplication;
using trading_engine::paper::PaperPortfolio;
using trading_engine::paper::PositionFill;
using trading_engine::paper::PositionLedger;
using trading_engine::paper::PositionLedgerApplyStatus;

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

PositionFill fill(
    std::string asset_id,
    std::uint32_t asset_index,
    OrderSide side,
    std::int64_t qty_lots,
    std::int64_t price_tick
) {
    PositionFill out;
    out.asset_id = std::move(asset_id);
    out.asset_index = asset_index;
    out.side = side;
    out.qty_lots = qty_lots;
    out.price_tick = price_tick;
    return out;
}

FillApplication buy_report_fill() {
    FillApplication out;
    out.asset_id = "asset_yes";
    out.asset_index = 17;
    out.side = OrderSide::Buy;
    out.report.status = ChildOrderStatus::Filled;
    out.report.filled_lots = 10;
    out.report.avg_fill_price_tick = 700;
    return out;
}

int test_buy_increases_qty() {
    PositionLedger ledger;
    const auto result = ledger.apply_fill(fill("asset_yes", 17, OrderSide::Buy, 10, 700));
    if (!result.applied || result.status != PositionLedgerApplyStatus::Applied) {
        return fail("buy fill should apply");
    }

    const auto* position = ledger.find("asset_yes");
    if (position == nullptr) {
        return fail("position missing");
    }
    if (const auto check = expect_equal(position->asset_id, std::string{"asset_yes"}, "asset");
        check != 0) {
        return check;
    }
    if (const auto check = expect_equal(position->asset_index, 17U, "asset index");
        check != 0) {
        return check;
    }
    return expect_equal(position->qty_lots, 10LL, "qty lots");
}

int test_buy_updates_avg_cost() {
    PositionLedger ledger;
    (void)ledger.apply_fill(fill("asset_yes", 17, OrderSide::Buy, 10, 700));
    (void)ledger.apply_fill(fill("asset_yes", 17, OrderSide::Buy, 5, 1000));

    if (const auto check =
            expect_equal(ledger.lots("asset_yes"), 15LL, "qty lots");
        check != 0) {
        return check;
    }
    if (const auto check =
            expect_equal(ledger.cost_basis_tick("asset_yes"), 12'000LL, "cost basis");
        check != 0) {
        return check;
    }
    return expect_equal(ledger.avg_cost_tick("asset_yes"), 800LL, "avg cost");
}

int test_rejects_unsupported_sell() {
    PositionLedger ledger;
    const auto result =
        ledger.apply_fill(fill("asset_yes", 17, OrderSide::Sell, 10, 700));
    if (result.applied ||
        result.status != PositionLedgerApplyStatus::UnsupportedSell) {
        return fail("sell should be unsupported in v0");
    }
    return expect_equal(ledger.asset_count(), static_cast<std::size_t>(0), "asset count");
}

int test_portfolio_buy_updates_exposure() {
    PaperPortfolio portfolio;
    const auto result = portfolio.apply_fill(buy_report_fill());
    if (!result.applied) {
        return fail("portfolio buy fill should apply");
    }
    portfolio.mark_mid("asset_yes", 800);
    portfolio.mark_liquidation("asset_yes", 650);

    const auto exposure = portfolio.exposure();
    if (const auto check =
            expect_equal(exposure.asset_count, static_cast<std::size_t>(1), "assets");
        check != 0) {
        return check;
    }
    if (const auto check =
            expect_equal(exposure.total_qty_lots, 10LL, "qty");
        check != 0) {
        return check;
    }
    if (const auto check =
            expect_equal(exposure.gross_cost_basis_tick, 7000LL, "cost basis");
        check != 0) {
        return check;
    }
    if (const auto check =
            expect_equal(exposure.unrealized_pnl_mid_tick, 1000LL, "mid pnl");
        check != 0) {
        return check;
    }
    return expect_equal(
        exposure.unrealized_pnl_liquidation_tick,
        -500LL,
        "liquidation pnl"
    );
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return fail("expected one test case name");
    }

    const std::string test_case{argv[1]};
    if (test_case == "PositionLedger_BuyIncreasesQty") {
        return test_buy_increases_qty();
    }
    if (test_case == "PositionLedger_BuyUpdatesAvgCost") {
        return test_buy_updates_avg_cost();
    }
    if (test_case == "PositionLedger_RejectsUnsupportedSellInV0") {
        return test_rejects_unsupported_sell();
    }
    if (test_case == "PaperPortfolio_BuyFillUpdatesExposure") {
        return test_portfolio_buy_updates_exposure();
    }

    return fail("unknown test case");
}
