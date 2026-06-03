#include "engine/paper/ledger/MakerFillApplication.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::execution::FillLiquidityRole;
using trading_engine::execution::MakerExecutionReport;
using trading_engine::execution::MakerQuoteStatus;
using trading_engine::execution::QuoteSide;
using trading_engine::paper::MakerFillApplication;
using trading_engine::paper::Side;

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

MakerExecutionReport filled_report() {
    MakerExecutionReport report;
    report.report_id = 1001;
    report.quote_id = 2001;
    report.approved_quote_id = 3001;
    report.quote_group_id = 4001;
    report.asset_index = 7;
    report.asset_id = "asset_yes";
    report.side = QuoteSide::Bid;
    report.status = MakerQuoteStatus::Filled;
    report.liquidity_role = FillLiquidityRole::Maker;
    report.filled_qty_lots = 100;
    report.avg_fill_price_tick = 490000;
    report.remaining_qty_lots = 0;
    report.exchange_ts_ns = 1000000000;
    report.recv_ts_ns = 1000000001;
    return report;
}

void MakerFillApplication_CreatesFillFromFilledMakerReport() {
    const auto result = MakerFillApplication{}.from_report(filled_report());
    expect_true(result.has_fill, "has fill");
    expect_true(result.fill.fill_id != 0, "fill id");
    expect_equal(result.fill.report_id, 1001ULL, "report id");
    expect_equal(result.fill.side, Side::Buy, "paper side");
    expect_equal(result.fill.qty_lots, 100LL, "qty");
    expect_equal(result.fill.fill_price_tick, 490000LL, "price");
    expect_equal(result.fill.liquidity_role, FillLiquidityRole::Maker, "role");
}

void MakerFillApplication_DoesNotCreateFillFromUnfilledReport() {
    auto report = filled_report();
    report.status = MakerQuoteStatus::ActivePaper;
    report.filled_qty_lots = 0;

    const auto result = MakerFillApplication{}.from_report(report);
    if (result.has_fill) {
        fail("unfilled report must not create fill");
    }
}

void MakerFillApplication_PreservesQuoteIds() {
    const auto result = MakerFillApplication{}.from_report(filled_report());
    expect_true(result.has_fill, "has fill");
    expect_equal(result.fill.quote_id, 2001ULL, "quote id");
    expect_equal(result.fill.approved_quote_id, 3001ULL, "approved quote id");
    expect_equal(result.fill.quote_group_id, 4001ULL, "quote group id");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "MakerFillApplication_CreatesFillFromFilledMakerReport",
            &MakerFillApplication_CreatesFillFromFilledMakerReport
        },
        {
            "MakerFillApplication_DoesNotCreateFillFromUnfilledReport",
            &MakerFillApplication_DoesNotCreateFillFromUnfilledReport
        },
        {
            "MakerFillApplication_PreservesQuoteIds",
            &MakerFillApplication_PreservesQuoteIds
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
