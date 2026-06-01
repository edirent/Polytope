#include "engine/execution/public/ExecutionTypes.h"
#include "engine/paper/ledger/CashLedger.h"
#include "engine/paper/ledger/FillApplication.h"
#include "engine/paper/pnl/MarkPriceProvider.h"
#include "engine/paper/pnl/PaperPnLEngine.h"
#include "engine/paper/portfolio/PaperPortfolio.h"
#include "engine/state/view/MarketDepthView.h"

#include <array>
#include <iostream>
#include <span>
#include <string>

namespace {

using trading_engine::execution::ChildOrderStatus;
using trading_engine::execution::OrderSide;
using trading_engine::paper::CashLedger;
using trading_engine::paper::FillApplication;
using trading_engine::paper::MarkPriceProvider;
using trading_engine::paper::MarkQuality;
using trading_engine::paper::PaperPnLEngine;
using trading_engine::paper::PaperPortfolio;
using trading_engine::state::MarketDepthView;
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

FillApplication buy_fill(
    std::uint32_t asset_index,
    std::int64_t qty_lots,
    std::int64_t price_tick
) {
    FillApplication fill;
    fill.asset_id = "asset_yes";
    fill.asset_index = asset_index;
    fill.side = OrderSide::Buy;
    fill.report.status = ChildOrderStatus::Filled;
    fill.report.filled_lots = qty_lots;
    fill.report.avg_fill_price_tick = price_tick;
    return fill;
}

MarketDepthView depth(
    std::uint32_t asset_index,
    std::int64_t bid_tick,
    std::int64_t ask_tick
) {
    MarketDepthView view;
    view.asset_index = asset_index;
    view.usable_for_depth = true;
    view.bid_count = 1;
    view.ask_count = 1;
    view.bids[0] = PriceLevel{bid_tick, 0.0, 10.0};
    view.asks[0] = PriceLevel{ask_tick, 0.0, 10.0};
    return view;
}

CashLedger cash_after_buy(std::int64_t starting_cash, std::int64_t cost) {
    CashLedger cash;
    cash.starting_cash_tick = starting_cash;
    cash.cash_tick = starting_cash - cost;
    return cash;
}

int test_marks_long_at_mid() {
    PaperPortfolio portfolio;
    (void)portfolio.apply_fill(buy_fill(17, 10, 700));

    const auto view = depth(17, 800, 1000);
    PaperPnLEngine engine;
    const auto result = engine.compute(
        portfolio,
        cash_after_buy(100'000, 7000),
        std::span<const MarketDepthView>(&view, 1)
    );

    return expect_equal(
        result.unrealized_pnl_mid_tick,
        2000LL,
        "unrealized pnl mid"
    );
}

int test_marks_long_at_liquidation_bid() {
    PaperPortfolio portfolio;
    (void)portfolio.apply_fill(buy_fill(17, 10, 700));

    const auto view = depth(17, 650, 950);
    PaperPnLEngine engine;
    const auto result = engine.compute(
        portfolio,
        cash_after_buy(100'000, 7000),
        std::span<const MarketDepthView>(&view, 1)
    );

    return expect_equal(
        result.unrealized_pnl_liquidation_tick,
        -500LL,
        "unrealized pnl liquidation"
    );
}

int test_missing_bid_is_degraded() {
    PaperPortfolio portfolio;
    (void)portfolio.apply_fill(buy_fill(17, 10, 700));

    MarketDepthView view;
    view.asset_index = 17;
    view.usable_for_depth = true;
    view.bid_count = 0;
    view.ask_count = 1;
    view.asks[0] = PriceLevel{900, 0.0, 10.0};

    const MarkPriceProvider provider;
    const auto mark = provider.mark_from_depth(view);
    if (const auto check =
            expect_equal(mark.liquidation_mark_tick, 0LL, "liquidation mark");
        check != 0) {
        return check;
    }
    if (mark.liquidation_quality != MarkQuality::MissingBid) {
        return fail("missing bid should be reported as MissingBid");
    }

    PaperPnLEngine engine;
    const auto result = engine.compute(
        portfolio,
        cash_after_buy(100'000, 7000),
        std::span<const MarketDepthView>(&view, 1)
    );
    if (result.worst_mark_quality == MarkQuality::Good) {
        return fail("missing bid should degrade pnl mark quality");
    }
    return 0;
}

int test_computes_equity_curve() {
    PaperPortfolio portfolio;
    (void)portfolio.apply_fill(buy_fill(17, 10, 700));

    const auto view = depth(17, 650, 950);
    PaperPnLEngine engine;
    const auto result = engine.compute(
        portfolio,
        cash_after_buy(100'000, 7000),
        std::span<const MarketDepthView>(&view, 1),
        1234
    );

    if (const auto check =
            expect_equal(result.equity.equity_mid_tick, 101'000LL, "equity mid");
        check != 0) {
        return check;
    }
    if (const auto check = expect_equal(
            result.equity.equity_liquidation_tick,
            99'500LL,
            "equity liquidation"
        );
        check != 0) {
        return check;
    }
    return expect_equal(result.equity.ts_ns, 1234ULL, "equity timestamp");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return fail("expected one test case name");
    }

    const std::string test_case{argv[1]};
    if (test_case == "PnL_MarksLongAtMid") {
        return test_marks_long_at_mid();
    }
    if (test_case == "PnL_MarksLongAtLiquidationBid") {
        return test_marks_long_at_liquidation_bid();
    }
    if (test_case == "PnL_MissingBidIsDegraded") {
        return test_missing_bid_is_degraded();
    }
    if (test_case == "PnL_ComputesEquityCurve") {
        return test_computes_equity_curve();
    }

    return fail("unknown test case");
}
