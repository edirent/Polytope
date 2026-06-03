#include "engine/paper/metrics/MakerPerformanceMetrics.h"

#include <array>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::paper::FillLiquidityRole;
using trading_engine::paper::MakerFillMetricInput;
using trading_engine::paper::MakerPerformanceMetrics;
using trading_engine::paper::MakerPerformanceMetricsInput;
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

PaperFill fill(
    std::uint64_t fill_id,
    Side side,
    std::int64_t price_tick
) {
    PaperFill out;
    out.fill_id = fill_id;
    out.asset_index = 7;
    out.asset_id = "asset_yes";
    out.side = side;
    out.liquidity_role = FillLiquidityRole::Maker;
    out.qty_lots = 100;
    out.fill_price_tick = price_tick;
    return out;
}

void MakerMetrics_ComputesSpreadCapture() {
    const std::array<MakerFillMetricInput, 2> fills{
        MakerFillMetricInput{fill(1, Side::Buy, 490000), 520000},
        MakerFillMetricInput{fill(2, Side::Sell, 530000), 520000}
    };

    MakerPerformanceMetricsInput input;
    input.fills = std::span<const MakerFillMetricInput>{fills};
    input.quote_count = 1;
    input.cancel_replace_count = 0;

    const auto metrics = MakerPerformanceMetrics{}.compute(input);
    expect_equal(metrics.maker_fill_count, 2ULL, "maker fills");
    expect_equal(metrics.maker_bid_fill_count, 1ULL, "bid fills");
    expect_equal(metrics.maker_ask_fill_count, 1ULL, "ask fills");
    expect_equal(metrics.spread_capture_tick, 4000000LL, "spread capture");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"MakerMetrics_ComputesSpreadCapture", &MakerMetrics_ComputesSpreadCapture},
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
