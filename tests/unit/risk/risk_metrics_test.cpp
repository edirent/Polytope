#include "engine/risk/metrics/RiskMetrics.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::risk::RiskMetrics;

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

void RiskMetrics_DefaultCountersZero() {
    const RiskMetrics metrics;

    expect_equal(metrics.evaluate_count, 0ULL, "evaluate_count");
    expect_equal(metrics.approve_count, 0ULL, "approve_count");
    expect_equal(metrics.reject_count, 0ULL, "reject_count");
    expect_equal(metrics.reject_invalid_intent, 0ULL, "invalid_intent");
    expect_equal(metrics.reject_expired, 0ULL, "expired");
    expect_equal(metrics.reject_duplicate, 0ULL, "duplicate");
    expect_equal(metrics.reject_kill_switch, 0ULL, "kill_switch");
    expect_equal(metrics.reject_stale_book, 0ULL, "stale_book");
    expect_equal(metrics.reject_insufficient_depth, 0ULL, "depth");
    expect_equal(metrics.reject_cost_drift, 0ULL, "cost_drift");
    expect_equal(metrics.reject_low_edge, 0ULL, "low_edge");
    expect_equal(metrics.reject_exposure, 0ULL, "exposure");
    expect_equal(metrics.reject_inventory, 0ULL, "inventory");
    expect_equal(metrics.reject_partial_fill, 0ULL, "partial_fill");
    expect_equal(metrics.reject_max_loss, 0ULL, "max_loss");
    expect_equal(metrics.reject_rate_limited, 0ULL, "rate_limited");
    expect_equal(metrics.reservation_created, 0ULL, "reservation_created");
    expect_equal(metrics.reservation_expired, 0ULL, "reservation_expired");
    expect_equal(metrics.reservation_released, 0ULL, "reservation_released");
    expect_equal(metrics.vwap_recomputed, 0ULL, "vwap_recomputed");
    expect_equal(metrics.evaluate_latency_ns.count, 0ULL, "latency count");
}

void RiskMetrics_HasCounterForEveryWorkflowRejectReason() {
    RiskMetrics metrics;

    ++metrics.reject_invalid_intent;
    ++metrics.reject_expired;
    ++metrics.reject_duplicate;
    ++metrics.reject_kill_switch;
    ++metrics.reject_stale_book;
    ++metrics.reject_insufficient_depth;
    ++metrics.reject_cost_drift;
    ++metrics.reject_low_edge;
    ++metrics.reject_exposure;
    ++metrics.reject_inventory;
    ++metrics.reject_partial_fill;
    ++metrics.reject_max_loss;
    ++metrics.reject_rate_limited;

    expect_equal(metrics.reject_invalid_intent, 1ULL, "invalid_intent");
    expect_equal(metrics.reject_expired, 1ULL, "expired");
    expect_equal(metrics.reject_duplicate, 1ULL, "duplicate");
    expect_equal(metrics.reject_kill_switch, 1ULL, "kill_switch");
    expect_equal(metrics.reject_stale_book, 1ULL, "stale_book");
    expect_equal(metrics.reject_insufficient_depth, 1ULL, "depth");
    expect_equal(metrics.reject_cost_drift, 1ULL, "cost_drift");
    expect_equal(metrics.reject_low_edge, 1ULL, "low_edge");
    expect_equal(metrics.reject_exposure, 1ULL, "exposure");
    expect_equal(metrics.reject_inventory, 1ULL, "inventory");
    expect_equal(metrics.reject_partial_fill, 1ULL, "partial_fill");
    expect_equal(metrics.reject_max_loss, 1ULL, "max_loss");
    expect_equal(metrics.reject_rate_limited, 1ULL, "rate_limited");
}

void RiskMetrics_ObserveEvaluateLatency() {
    RiskMetrics metrics;

    metrics.observe_evaluate_latency(30);
    metrics.observe_evaluate_latency(10);
    metrics.observe_evaluate_latency(50);

    expect_equal(metrics.evaluate_latency_ns.count, 3ULL, "count");
    expect_equal(metrics.evaluate_latency_ns.last_ns, 50ULL, "last");
    expect_equal(metrics.evaluate_latency_ns.min_ns, 10ULL, "min");
    expect_equal(metrics.evaluate_latency_ns.max_ns, 50ULL, "max");
    expect_equal(metrics.evaluate_latency_ns.total_ns, 90ULL, "total");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"RiskMetrics_DefaultCountersZero", &RiskMetrics_DefaultCountersZero},
        {
            "RiskMetrics_HasCounterForEveryWorkflowRejectReason",
            &RiskMetrics_HasCounterForEveryWorkflowRejectReason
        },
        {"RiskMetrics_ObserveEvaluateLatency", &RiskMetrics_ObserveEvaluateLatency}
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
    if (argc == 2) {
        return run_test(argv[1]);
    }

    int failures = 0;
    for (const auto& [name, _] : tests()) {
        failures += run_test(name) == 0 ? 0 : 1;
    }
    return failures == 0 ? 0 : 1;
}
