#include "engine/paper/ledger/PaperLedger.h"
#include "engine/paper/pnl/MakerPnLEngine.h"
#include "engine/state/view/MarketDepthView.h"

#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::paper::FillLiquidityRole;
using trading_engine::paper::MakerPnLEngine;
using trading_engine::paper::MarkQuality;
using trading_engine::paper::PaperFill;
using trading_engine::paper::PaperLedger;
using trading_engine::paper::Side;
using trading_engine::state::MarketDepthView;
using trading_engine::state::PriceLevel;

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

PaperFill maker_buy() {
    PaperFill fill;
    fill.fill_id = 1;
    fill.report_id = 10;
    fill.asset_index = 7;
    fill.asset_id = "asset_yes";
    fill.side = Side::Buy;
    fill.liquidity_role = FillLiquidityRole::Maker;
    fill.qty_lots = 100;
    fill.fill_price_tick = 490000;
    fill.idempotency_hash = fill.fill_id;
    return fill;
}

MarketDepthView depth(
    std::int64_t bid_tick,
    std::int64_t ask_tick,
    bool usable = true
) {
    MarketDepthView view;
    view.asset_index = 7;
    view.usable_for_depth = usable;
    if (bid_tick > 0) {
        view.bid_count = 1;
        view.bids[0] = PriceLevel{bid_tick, 0.0, 100.0};
    }
    if (ask_tick > 0) {
        view.ask_count = 1;
        view.asks[0] = PriceLevel{ask_tick, 0.0, 100.0};
    }
    return view;
}

PaperLedger ledger_with_long() {
    PaperLedger ledger{100000000000};
    (void)ledger.apply_fill(maker_buy());
    return ledger;
}

void MakerPnL_MarksLongAtMid() {
    auto ledger = ledger_with_long();
    const auto view = depth(510000, 530000);
    const auto pnl = MakerPnLEngine{}.compute(
        ledger,
        std::span<const MarketDepthView>(&view, 1),
        123
    );
    expect_equal(
        pnl.maker_unrealized_pnl_mid_tick,
        3000000LL,
        "mid unrealized"
    );
    expect_equal(pnl.equity_mid_tick, 100003000000LL, "equity mid");
}

void MakerPnL_MarksLongAtLiquidationBid() {
    auto ledger = ledger_with_long();
    const auto view = depth(505000, 530000);
    const auto pnl = MakerPnLEngine{}.compute(
        ledger,
        std::span<const MarketDepthView>(&view, 1)
    );
    expect_equal(
        pnl.maker_unrealized_pnl_liquidation_tick,
        1500000LL,
        "liquidation unrealized"
    );
}

void MakerPnL_MissingBidLiquidationMarkZero() {
    auto ledger = ledger_with_long();
    const auto view = depth(0, 530000);
    const auto pnl = MakerPnLEngine{}.compute(
        ledger,
        std::span<const MarketDepthView>(&view, 1)
    );
    expect_equal(
        pnl.maker_unrealized_pnl_liquidation_tick,
        -49000000LL,
        "missing bid liquidation"
    );
    expect_equal(pnl.mark_quality, MarkQuality::MissingBid, "mark quality");
}

void MakerPnL_DegradedBookDoesNotProduceGoodMark() {
    auto ledger = ledger_with_long();
    auto view = depth(510000, 530000, false);
    view.recovering = true;
    const auto pnl = MakerPnLEngine{}.compute(
        ledger,
        std::span<const MarketDepthView>(&view, 1)
    );
    if (pnl.mark_quality == MarkQuality::Good) {
        fail("degraded book must not produce Good mark");
    }
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"MakerPnL_MarksLongAtMid", &MakerPnL_MarksLongAtMid},
        {
            "MakerPnL_MarksLongAtLiquidationBid",
            &MakerPnL_MarksLongAtLiquidationBid
        },
        {
            "MakerPnL_MissingBidLiquidationMarkZero",
            &MakerPnL_MissingBidLiquidationMarkZero
        },
        {
            "MakerPnL_DegradedBookDoesNotProduceGoodMark",
            &MakerPnL_DegradedBookDoesNotProduceGoodMark
        },
    };
    return test_map;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "expected test name\n";
        return 1;
    }
    const auto it = tests().find(argv[1]);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << argv[1] << "\n";
        return 1;
    }
    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}
