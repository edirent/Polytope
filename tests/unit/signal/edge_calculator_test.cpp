#include "engine/signal/edge/TheoreticalEdgeCalculator.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::oracle::CandidateBundle;
using trading_engine::signal::CostResult;
using trading_engine::signal::FeeModel;
using trading_engine::signal::LatencyBufferModel;
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

CostResult cost(std::int64_t total_cost_tick) {
    CostResult out;
    out.enough_depth = true;
    out.total_cost_tick = total_cost_tick;
    return out;
}

TheoreticalEdgeCalculator calculator(
    std::int64_t fee_tick,
    std::int64_t latency_buffer_tick
) {
    return TheoreticalEdgeCalculator{
        FeeModel{fee_tick},
        LatencyBufferModel{latency_buffer_tick}
    };
}

void EdgeCalculator_ComputesPositiveEdge() {
    const auto edge = calculator(0, 0).calculate(
        bundle(1'000'000),
        cost(900'000)
    );

    expect_equal(edge.estimated_edge_tick, 100'000LL, "edge");
    expect_true(edge.above_threshold, "above threshold");
}

void EdgeCalculator_SubtractsFee() {
    const auto edge = calculator(10'000, 0).calculate(
        bundle(1'000'000),
        cost(900'000)
    );

    expect_equal(edge.fee_tick, 10'000LL, "fee");
    expect_equal(edge.estimated_edge_tick, 90'000LL, "edge");
}

void EdgeCalculator_SubtractsLatencyBuffer() {
    const auto edge = calculator(0, 25'000).calculate(
        bundle(1'000'000),
        cost(900'000)
    );

    expect_equal(edge.latency_buffer_tick, 25'000LL, "latency");
    expect_equal(edge.estimated_edge_tick, 75'000LL, "edge");
}

void EdgeCalculator_RejectsBelowMinEdge() {
    const auto edge = calculator(10'000, 10'000).calculate(
        bundle(1'000'000, 90'000),
        cost(900'000)
    );

    expect_equal(edge.estimated_edge_tick, 80'000LL, "edge");
    expect_equal(edge.min_edge_tick, 90'000LL, "min edge");
    expect_false(edge.above_threshold, "above threshold");
}

void EdgeCalculator_HandlesZeroCost() {
    const auto edge = calculator(0, 0).calculate(
        bundle(1'000'000),
        cost(0)
    );

    expect_equal(edge.total_cost_tick, 0LL, "cost");
    expect_equal(edge.estimated_edge_tick, 1'000'000LL, "edge");
    expect_true(edge.above_threshold, "above threshold");
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
        {"EdgeCalculator_HandlesZeroCost", &EdgeCalculator_HandlesZeroCost}
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
