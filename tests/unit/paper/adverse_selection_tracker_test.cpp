#include "engine/paper/pnl/AdverseSelectionTracker.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::paper::AdverseSelectionStatus;
using trading_engine::paper::AdverseSelectionTracker;
using trading_engine::paper::FillLiquidityRole;
using trading_engine::paper::PaperFill;
using trading_engine::paper::Side;

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

PaperFill fill(Side side) {
    PaperFill out;
    out.fill_id = side == Side::Buy ? 1 : 2;
    out.asset_index = 7;
    out.asset_id = "asset_yes";
    out.side = side;
    out.liquidity_role = FillLiquidityRole::Maker;
    out.qty_lots = 100;
    out.fill_price_tick = side == Side::Buy ? 490000 : 530000;
    out.ts_ns = 1'000'000'000;
    return out;
}

void AdverseSelection_BidFillDownMoveNegative() {
    AdverseSelectionTracker tracker;
    const auto paper_fill = fill(Side::Buy);
    tracker.observe_fill(paper_fill, 520000);
    tracker.observe_mark(7, 6'000'000'000, 515000);

    const auto* record = tracker.find(paper_fill.fill_id);
    if (record == nullptr) {
        fail("missing adverse selection record");
    }
    expect_equal(record->status_5s, AdverseSelectionStatus::Ready, "status");
    expect_equal(record->adverse_selection_5s_tick, -5000LL, "5s move");
}

void AdverseSelection_AskFillUpMoveNegative() {
    AdverseSelectionTracker tracker;
    const auto paper_fill = fill(Side::Sell);
    tracker.observe_fill(paper_fill, 520000);
    tracker.observe_mark(7, 6'000'000'000, 525000);

    const auto* record = tracker.find(paper_fill.fill_id);
    if (record == nullptr) {
        fail("missing adverse selection record");
    }
    expect_equal(record->status_5s, AdverseSelectionStatus::Ready, "status");
    expect_equal(record->adverse_selection_5s_tick, -5000LL, "5s move");
}

void AdverseSelection_PendingWhenHorizonMissing() {
    AdverseSelectionTracker tracker;
    const auto paper_fill = fill(Side::Buy);
    tracker.observe_fill(paper_fill, 520000);
    tracker.observe_mark(7, 2'000'000'000, 515000);

    const auto* record = tracker.find(paper_fill.fill_id);
    if (record == nullptr) {
        fail("missing adverse selection record");
    }
    expect_equal(
        record->status_5s,
        AdverseSelectionStatus::PendingHorizon,
        "pending status"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "AdverseSelection_BidFillDownMoveNegative",
            &AdverseSelection_BidFillDownMoveNegative
        },
        {
            "AdverseSelection_AskFillUpMoveNegative",
            &AdverseSelection_AskFillUpMoveNegative
        },
        {
            "AdverseSelection_PendingWhenHorizonMissing",
            &AdverseSelection_PendingWhenHorizonMissing
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
