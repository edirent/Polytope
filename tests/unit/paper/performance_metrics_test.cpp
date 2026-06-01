#include "engine/paper/metrics/DrawdownCalculator.h"
#include "engine/paper/metrics/PerformanceMetricsEngine.h"
#include "engine/paper/metrics/SharpeCalculator.h"

#include <array>
#include <cmath>
#include <iostream>
#include <span>
#include <string>
#include <type_traits>

namespace {

using trading_engine::paper::DrawdownCalculator;
using trading_engine::paper::EquityCurve;
using trading_engine::paper::MetricStatus;
using trading_engine::paper::PerformanceMetricsEngine;
using trading_engine::paper::PerformanceMetricsInput;
using trading_engine::paper::SharpeCalculator;

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

template <typename T, typename U>
int expect_equal(const T& actual, const U& expected, const char* message) {
    if (!(actual == expected)) {
        std::cerr << message << ": expected ";
        if constexpr (std::is_enum_v<U>) {
            std::cerr << static_cast<int>(expected);
        } else {
            std::cerr << expected;
        }
        std::cerr << ", got ";
        if constexpr (std::is_enum_v<T>) {
            std::cerr << static_cast<int>(actual);
        } else {
            std::cerr << actual;
        }
        std::cerr << '\n';
        return 1;
    }
    return 0;
}

int expect_near(
    double actual,
    double expected,
    double tolerance,
    const char* message
) {
    if (std::fabs(actual - expected) > tolerance) {
        std::cerr << message << ": expected " << expected << ", got " << actual
                  << '\n';
        return 1;
    }
    return 0;
}

EquityCurve equity(std::int64_t value) {
    EquityCurve curve;
    curve.equity_mid_tick = value;
    curve.equity_liquidation_tick = value;
    return curve;
}

int test_sharpe_insufficient_samples() {
    const std::array<double, 1> returns{0.01};
    const auto result = SharpeCalculator{2}.compute(returns);
    return expect_equal(
        result.status,
        MetricStatus::InsufficientData,
        "sharpe status"
    );
}

int test_sharpe_zero_vol_returns_insufficient() {
    const std::array<double, 3> returns{0.01, 0.01, 0.01};
    const auto result = SharpeCalculator{2}.compute(returns);
    return expect_equal(
        result.status,
        MetricStatus::InsufficientData,
        "sharpe zero volatility status"
    );
}

int test_drawdown_computes_max_drawdown() {
    const std::array<std::int64_t, 5> equity_ticks{
        100'000,
        120'000,
        90'000,
        110'000,
        80'000
    };
    const auto result = DrawdownCalculator{}.compute(equity_ticks);
    if (const auto check = expect_equal(
            result.status,
            MetricStatus::Ok,
            "drawdown status"
        );
        check != 0) {
        return check;
    }
    return expect_equal(result.max_drawdown_tick, 40'000LL, "max drawdown");
}

int test_performance_computes_fill_rate() {
    const std::array<EquityCurve, 3> curve{
        equity(100'000),
        equity(101'000),
        equity(99'000)
    };

    PerformanceMetricsInput input;
    input.equity_curve = std::span<const EquityCurve>{curve};
    input.intents_observed = 10;
    input.approvals_observed = 4;
    input.plans_created = 5;
    input.filled_plans = 3;
    input.filled_notional_tick = 20'000;

    const auto result = PerformanceMetricsEngine{}.compute(input);
    if (const auto check =
            expect_equal(result.fill_rate.status, MetricStatus::Ok, "fill status");
        check != 0) {
        return check;
    }
    if (const auto check =
            expect_near(result.fill_rate.value, 0.6, 1e-12, "fill rate");
        check != 0) {
        return check;
    }
    if (const auto check = expect_near(
            result.risk_approval_rate.value,
            0.4,
            1e-12,
            "risk approval rate"
        );
        check != 0) {
        return check;
    }
    return expect_near(
        result.intent_conversion_rate.value,
        0.5,
        1e-12,
        "intent conversion rate"
    );
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return fail("expected one test case name");
    }

    const std::string test_case{argv[1]};
    if (test_case == "Sharpe_InsufficientSamples") {
        return test_sharpe_insufficient_samples();
    }
    if (test_case == "Sharpe_ZeroVolReturnsInsufficient") {
        return test_sharpe_zero_vol_returns_insufficient();
    }
    if (test_case == "Drawdown_ComputesMaxDrawdown") {
        return test_drawdown_computes_max_drawdown();
    }
    if (test_case == "Performance_ComputesFillRate") {
        return test_performance_computes_fill_rate();
    }

    return fail("unknown test case");
}
