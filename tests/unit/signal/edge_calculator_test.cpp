#include "engine/signal/edge/TheoreticalEdgeCalculator.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::oracle::CandidateBundle;
using trading_engine::signal::CostResult;
using trading_engine::signal::EdgeFailureReason;
using trading_engine::signal::EdgeBreakdown;
using trading_engine::signal::FeeModel;
using trading_engine::signal::LatencyBufferModel;
using trading_engine::signal::SignalConfig;
using trading_engine::signal::SlippageBufferModel;
using trading_engine::signal::TheoreticalEdgeCalculator;

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

CandidateBundle bundle(
    std::int64_t payout_tick,
    std::int64_t min_edge_tick = 0
) {
    CandidateBundle out;
    out.bundle_id = 1;
    out.guaranteed_payout_tick = payout_tick;
    out.min_edge_tick = min_edge_tick;
    return out;
}

CostResult cost(
    std::int64_t cost_per_bundle_tick,
    std::int64_t bundle_qty = 1
) {
    CostResult out;
    out.executable = true;
    out.bundle_qty = bundle_qty;
    out.avg_cost_tick = cost_per_bundle_tick;
    out.total_cost_tick = cost_per_bundle_tick * bundle_qty;
    return out;
}

TheoreticalEdgeCalculator calculator(
    std::int64_t fee_tick,
    std::int64_t latency_buffer_tick,
    std::int64_t slippage_buffer_tick = 0
) {
    return TheoreticalEdgeCalculator{
        FeeModel{fee_tick},
        LatencyBufferModel{latency_buffer_tick},
        SlippageBufferModel{slippage_buffer_tick}
    };
}

void EdgeCalculator_ComputesPositiveEdge() {
    const auto edge = calculator(0, 0).calculate(
        bundle(1'000'000),
        cost(900'000)
    );

    expect_equal(edge.unit_edge_tick, 100'000LL, "unit edge");
    expect_equal(edge.total_edge_tick, 100'000LL, "total edge");
    expect_true(edge.passed, "passed");
}

void EdgeCalculator_SubtractsFee() {
    const auto edge = calculator(10'000, 0).calculate(
        bundle(1'000'000),
        cost(900'000)
    );

    expect_equal(edge.fee_per_bundle_tick, 10'000LL, "fee");
    expect_equal(edge.unit_edge_tick, 90'000LL, "edge");
}

void EdgeCalculator_SubtractsLatencyBuffer() {
    const auto edge = calculator(0, 25'000).calculate(
        bundle(1'000'000),
        cost(900'000)
    );

    expect_equal(
        edge.latency_buffer_per_bundle_tick,
        25'000LL,
        "latency"
    );
    expect_equal(edge.unit_edge_tick, 75'000LL, "edge");
}

void EdgeCalculator_RejectsBelowMinEdge() {
    const auto edge = calculator(10'000, 10'000).calculate(
        bundle(1'000'000, 90'000),
        cost(900'000)
    );

    expect_equal(edge.unit_edge_tick, 80'000LL, "edge");
    expect_equal(
        edge.failure_reason,
        EdgeFailureReason::BelowMinUnitEdge,
        "failure"
    );
    expect_false(edge.passed, "passed");
}

void EdgeCalculator_HandlesZeroCost() {
    const auto edge = calculator(0, 0).calculate(
        bundle(1'000'000),
        cost(0)
    );

    expect_equal(edge.vwap_cost_per_bundle_tick, 0LL, "cost");
    expect_equal(edge.unit_edge_tick, 1'000'000LL, "edge");
    expect_true(edge.passed, "passed");
}

void EdgeCalculator_ComputesUnitEdge() {
    const auto edge = calculator(10'000, 20'000, 5'000).calculate(
        bundle(1'000'000),
        cost(900'000)
    );

    expect_equal(edge.guaranteed_payout_per_bundle_tick, 1'000'000LL, "payout");
    expect_equal(edge.vwap_cost_per_bundle_tick, 900'000LL, "cost");
    expect_equal(edge.unit_edge_tick, 65'000LL, "unit edge");
}

void EdgeCalculator_ComputesTotalEdgeWithBundleQty() {
    const auto edge = calculator(0, 0).calculate(
        bundle(1'000'000),
        cost(900'000, 7)
    );

    expect_equal(edge.bundle_qty, 7LL, "bundle qty");
    expect_equal(edge.unit_edge_tick, 100'000LL, "unit edge");
    expect_equal(edge.total_edge_tick, 700'000LL, "total edge");
}

void EdgeCalculator_RejectsBelowMinUnitEdge() {
    SignalConfig config;
    config.min_unit_edge_tick = 120'000;

    const auto edge = calculator(0, 0).calculate(
        bundle(1'000'000),
        cost(900'000, 10),
        config
    );

    expect_equal(edge.unit_edge_tick, 100'000LL, "unit edge");
    expect_equal(
        edge.failure_reason,
        EdgeFailureReason::BelowMinUnitEdge,
        "failure"
    );
    expect_false(edge.passed, "passed");
}

void EdgeCalculator_RejectsBelowMinTotalEdge() {
    SignalConfig config;
    config.min_total_edge_tick = 1'100'000;

    const auto edge = calculator(0, 0).calculate(
        bundle(1'000'000),
        cost(900'000, 10),
        config
    );

    expect_equal(edge.total_edge_tick, 1'000'000LL, "total edge");
    expect_equal(
        edge.failure_reason,
        EdgeFailureReason::BelowMinTotalEdge,
        "failure"
    );
    expect_false(edge.passed, "passed");
}

void EdgeCalculator_RejectsBelowMinEdgeBps() {
    SignalConfig config;
    config.min_edge_bps = 1'200;

    const auto edge = calculator(0, 0).calculate(
        bundle(1'000'000),
        cost(900'000, 10),
        config
    );

    expect_equal(edge.edge_bps, 1'111LL, "edge bps");
    expect_equal(
        edge.failure_reason,
        EdgeFailureReason::BelowMinEdgeBps,
        "failure"
    );
    expect_false(edge.passed, "passed");
}

void EdgeCalculator_RejectsBelowMinBundleQty() {
    SignalConfig config;
    config.min_bundle_qty = 11;

    const auto edge = calculator(0, 0).calculate(
        bundle(1'000'000),
        cost(900'000, 10),
        config
    );

    expect_equal(edge.bundle_qty, 10LL, "bundle qty");
    expect_equal(
        edge.failure_reason,
        EdgeFailureReason::BelowMinBundleQty,
        "failure"
    );
    expect_false(edge.passed, "passed");
}

void EdgeCalculator_SubtractsSlippageBuffer() {
    SignalConfig config;
    config.slippage_buffer_tick = 15'000;

    const auto edge = calculator(0, 0).calculate(
        bundle(1'000'000),
        cost(900'000),
        config
    );

    expect_equal(
        edge.slippage_buffer_per_bundle_tick,
        15'000LL,
        "slippage"
    );
    expect_equal(edge.unit_edge_tick, 85'000LL, "unit edge");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "EdgeCalculator_ComputesPositiveEdge",
            &EdgeCalculator_ComputesPositiveEdge
        },
        {"EdgeCalculator_SubtractsFee", &EdgeCalculator_SubtractsFee},
        {
            "EdgeCalculator_SubtractsLatencyBuffer",
            &EdgeCalculator_SubtractsLatencyBuffer
        },
        {
            "EdgeCalculator_RejectsBelowMinEdge",
            &EdgeCalculator_RejectsBelowMinEdge
        },
        {"EdgeCalculator_HandlesZeroCost", &EdgeCalculator_HandlesZeroCost},
        {
            "EdgeCalculator_ComputesUnitEdge",
            &EdgeCalculator_ComputesUnitEdge
        },
        {
            "EdgeCalculator_ComputesTotalEdgeWithBundleQty",
            &EdgeCalculator_ComputesTotalEdgeWithBundleQty
        },
        {
            "EdgeCalculator_RejectsBelowMinUnitEdge",
            &EdgeCalculator_RejectsBelowMinUnitEdge
        },
        {
            "EdgeCalculator_RejectsBelowMinTotalEdge",
            &EdgeCalculator_RejectsBelowMinTotalEdge
        },
        {
            "EdgeCalculator_RejectsBelowMinEdgeBps",
            &EdgeCalculator_RejectsBelowMinEdgeBps
        },
        {
            "EdgeCalculator_RejectsBelowMinBundleQty",
            &EdgeCalculator_RejectsBelowMinBundleQty
        },
        {
            "EdgeCalculator_SubtractsSlippageBuffer",
            &EdgeCalculator_SubtractsSlippageBuffer
        }
    };
    return test_map;
}

int run_test(const std::string& name) {
    const auto it = tests().find(name);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << name << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << name << " failed: " << error.what() << '\n';
        return 1;
    }

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <test-name>\n";
        return 2;
    }
    return run_test(argv[1]);
}
