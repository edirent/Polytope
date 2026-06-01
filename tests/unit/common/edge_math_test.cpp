#include "engine/common/math/EdgeMath.h"

#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::common::math::EdgeMathInput;
using trading_engine::common::math::EdgeMathResult;
using trading_engine::common::math::compute_edge;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

void expect_false(bool value, const std::string& field) {
    if (value) {
        fail("expected false: " + field);
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

void expect_same_edge(
    const EdgeMathResult& actual,
    const EdgeMathResult& expected
) {
    expect_equal(actual.ok, expected.ok, "ok");
    expect_equal(actual.unit_edge_tick, expected.unit_edge_tick, "unit edge");
    expect_equal(actual.total_edge_tick, expected.total_edge_tick, "total edge");
    expect_equal(actual.edge_bps, expected.edge_bps, "edge bps");
}

EdgeMathInput baseline_input() {
    return EdgeMathInput{
        .guaranteed_payout_tick = 1'000'000,
        .total_cost_tick = 900'000,
        .fee_tick = 10'000,
        .latency_buffer_tick = 20'000,
        .slippage_buffer_tick = 5'000,
        .bundle_qty = 7
    };
}

void EdgeMath_ComputesUnitTotalAndBps() {
    const auto result = compute_edge(baseline_input());

    expect_true(result.ok, "ok");
    expect_equal(result.unit_edge_tick, 65'000LL, "unit edge");
    expect_equal(result.total_edge_tick, 455'000LL, "total edge");
    expect_equal(result.edge_bps, 722LL, "edge bps");
}

void EdgeMath_GenericEqualsFastInput() {
    const auto generic = compute_edge(baseline_input());
    const auto fast = compute_edge(EdgeMathInput{
        .guaranteed_payout_tick = 1'000'000,
        .total_cost_tick = 900'000,
        .fee_tick = 10'000,
        .latency_buffer_tick = 20'000,
        .slippage_buffer_tick = 5'000,
        .bundle_qty = 7
    });

    expect_same_edge(fast, generic);
}

void EdgeMath_HandlesZeroCostPositiveEdge() {
    const auto result = compute_edge(EdgeMathInput{
        .guaranteed_payout_tick = 1'000'000,
        .total_cost_tick = 0,
        .fee_tick = 0,
        .latency_buffer_tick = 0,
        .slippage_buffer_tick = 0,
        .bundle_qty = 1
    });

    expect_true(result.ok, "ok");
    expect_equal(result.unit_edge_tick, 1'000'000LL, "unit edge");
    expect_equal(result.total_edge_tick, 1'000'000LL, "total edge");
    expect_equal(
        result.edge_bps,
        std::numeric_limits<std::int64_t>::max(),
        "edge bps"
    );
}

void EdgeMath_RejectsOverflow() {
    const auto result = compute_edge(EdgeMathInput{
        .guaranteed_payout_tick = std::numeric_limits<std::int64_t>::max(),
        .total_cost_tick = -1,
        .fee_tick = 0,
        .latency_buffer_tick = 0,
        .slippage_buffer_tick = 0,
        .bundle_qty = 1
    });

    expect_false(result.ok, "ok");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"EdgeMath_ComputesUnitTotalAndBps",
         &EdgeMath_ComputesUnitTotalAndBps},
        {"EdgeMath_GenericEqualsFastInput", &EdgeMath_GenericEqualsFastInput},
        {"EdgeMath_HandlesZeroCostPositiveEdge",
         &EdgeMath_HandlesZeroCostPositiveEdge},
        {"EdgeMath_RejectsOverflow", &EdgeMath_RejectsOverflow},
    };
    return test_map;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <test-name>\n";
        return 2;
    }

    const auto it = tests().find(argv[1]);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << argv[1] << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << argv[1] << " failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
