#include "engine/execution/metrics/ExecutionMetrics.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::execution::ExecutionMetrics;

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

void ExecutionMetrics_DefaultCountersZero() {
    const ExecutionMetrics metrics;

    expect_equal(metrics.plan_created, 0ULL, "plan created");
    expect_equal(metrics.plan_submitted, 0ULL, "plan submitted");
    expect_equal(metrics.plan_filled, 0ULL, "plan filled");
    expect_equal(metrics.plan_failed, 0ULL, "plan failed");
    expect_equal(metrics.plan_expired, 0ULL, "plan expired");
    expect_equal(metrics.plan_hedge_required, 0ULL, "plan hedge required");
    expect_equal(metrics.child_created, 0ULL, "child created");
    expect_equal(metrics.child_filled, 0ULL, "child filled");
    expect_equal(metrics.child_partial, 0ULL, "child partial");
    expect_equal(metrics.child_cancelled, 0ULL, "child cancelled");
    expect_equal(metrics.child_failed, 0ULL, "child failed");
    expect_equal(metrics.report_published, 0ULL, "report published");
    expect_equal(metrics.reservation_consume, 0ULL, "reservation consume");
    expect_equal(metrics.reservation_release, 0ULL, "reservation release");
    expect_equal(metrics.reservation_expire, 0ULL, "reservation expire");
    expect_equal(
        metrics.reservation_hedge_required,
        0ULL,
        "reservation hedge required"
    );
    expect_equal(metrics.submit_latency_ns.count, 0ULL, "submit latency");
    expect_equal(
        metrics.fill_simulation_latency_ns.count,
        0ULL,
        "fill simulation latency"
    );
}

void ExecutionMetrics_HasPlanCounters() {
    ExecutionMetrics metrics;

    ++metrics.plan_created;
    ++metrics.plan_submitted;
    ++metrics.plan_filled;
    ++metrics.plan_failed;
    ++metrics.plan_expired;
    ++metrics.plan_hedge_required;

    expect_equal(metrics.plan_created, 1ULL, "plan created");
    expect_equal(metrics.plan_submitted, 1ULL, "plan submitted");
    expect_equal(metrics.plan_filled, 1ULL, "plan filled");
    expect_equal(metrics.plan_failed, 1ULL, "plan failed");
    expect_equal(metrics.plan_expired, 1ULL, "plan expired");
    expect_equal(metrics.plan_hedge_required, 1ULL, "plan hedge required");
}

void ExecutionMetrics_HasChildCounters() {
    ExecutionMetrics metrics;

    ++metrics.child_created;
    ++metrics.child_filled;
    ++metrics.child_partial;
    ++metrics.child_cancelled;
    ++metrics.child_failed;

    expect_equal(metrics.child_created, 1ULL, "child created");
    expect_equal(metrics.child_filled, 1ULL, "child filled");
    expect_equal(metrics.child_partial, 1ULL, "child partial");
    expect_equal(metrics.child_cancelled, 1ULL, "child cancelled");
    expect_equal(metrics.child_failed, 1ULL, "child failed");
}

void ExecutionMetrics_HasReportAndReservationCounters() {
    ExecutionMetrics metrics;

    ++metrics.report_published;
    ++metrics.reservation_consume;
    ++metrics.reservation_release;
    ++metrics.reservation_expire;
    ++metrics.reservation_hedge_required;

    expect_equal(metrics.report_published, 1ULL, "report published");
    expect_equal(metrics.reservation_consume, 1ULL, "reservation consume");
    expect_equal(metrics.reservation_release, 1ULL, "reservation release");
    expect_equal(metrics.reservation_expire, 1ULL, "reservation expire");
    expect_equal(
        metrics.reservation_hedge_required,
        1ULL,
        "reservation hedge required"
    );
}

void ExecutionMetrics_ObserveSubmitLatency() {
    ExecutionMetrics metrics;

    metrics.observe_submit_latency(10);
    metrics.observe_submit_latency(4);
    metrics.observe_submit_latency(20);

    expect_equal(metrics.submit_latency_ns.count, 3ULL, "count");
    expect_equal(metrics.submit_latency_ns.last_ns, 20ULL, "last");
    expect_equal(metrics.submit_latency_ns.min_ns, 4ULL, "min");
    expect_equal(metrics.submit_latency_ns.max_ns, 20ULL, "max");
    expect_equal(metrics.submit_latency_ns.total_ns, 34ULL, "total");
}

void ExecutionMetrics_ObserveFillSimulationLatency() {
    ExecutionMetrics metrics;

    metrics.observe_fill_simulation_latency(30);
    metrics.observe_fill_simulation_latency(12);

    expect_equal(metrics.fill_simulation_latency_ns.count, 2ULL, "count");
    expect_equal(metrics.fill_simulation_latency_ns.last_ns, 12ULL, "last");
    expect_equal(metrics.fill_simulation_latency_ns.min_ns, 12ULL, "min");
    expect_equal(metrics.fill_simulation_latency_ns.max_ns, 30ULL, "max");
    expect_equal(metrics.fill_simulation_latency_ns.total_ns, 42ULL, "total");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "ExecutionMetrics_DefaultCountersZero",
            &ExecutionMetrics_DefaultCountersZero
        },
        {
            "ExecutionMetrics_HasPlanCounters",
            &ExecutionMetrics_HasPlanCounters
        },
        {
            "ExecutionMetrics_HasChildCounters",
            &ExecutionMetrics_HasChildCounters
        },
        {
            "ExecutionMetrics_HasReportAndReservationCounters",
            &ExecutionMetrics_HasReportAndReservationCounters
        },
        {
            "ExecutionMetrics_ObserveSubmitLatency",
            &ExecutionMetrics_ObserveSubmitLatency
        },
        {
            "ExecutionMetrics_ObserveFillSimulationLatency",
            &ExecutionMetrics_ObserveFillSimulationLatency
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

    std::cout << name << " passed\n";
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
