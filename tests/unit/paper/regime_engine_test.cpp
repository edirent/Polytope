#include "engine/execution/public/ExecutionReport.h"
#include "engine/paper/regime/RegimeEngine.h"
#include "engine/risk/public/RiskDecision.h"
#include "engine/state/view/MarketDepthView.h"

#include <array>
#include <iostream>
#include <span>
#include <string>
#include <type_traits>

namespace {

using trading_engine::execution::ChildOrderStatus;
using trading_engine::execution::ExecutionReport;
using trading_engine::paper::DataRegime;
using trading_engine::paper::ExecutionRegime;
using trading_engine::paper::LiquidityRegime;
using trading_engine::paper::RegimeEngine;
using trading_engine::paper::RegimeInput;
using trading_engine::paper::RiskRegime;
using trading_engine::risk::RiskDecision;
using trading_engine::risk::RiskDecisionStatus;
using trading_engine::risk::RiskRejectReason;
using trading_engine::state::MarketDepthView;

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

MarketDepthView healthy_depth() {
    MarketDepthView view;
    view.asset_index = 7;
    view.usable_for_depth = true;
    view.last_ws_recv_ns = 1'000;
    view.bid_count = 1;
    view.ask_count = 1;
    return view;
}

RiskDecision rejected_decision() {
    RiskDecision decision;
    decision.status = RiskDecisionStatus::Rejected;
    decision.reject_reason = RiskRejectReason::TotalExposureLimit;
    return decision;
}

int test_healthy_when_all_ok() {
    const auto view = healthy_depth();
    RegimeInput input;
    input.now_ns = 1'100;
    input.depth_views = std::span<const MarketDepthView>{&view, 1};
    input.signal_intents_observed = 1;

    const auto result = RegimeEngine{}.classify(input);
    if (const auto check =
            expect_equal(result.data, DataRegime::Healthy, "data regime");
        check != 0) {
        return check;
    }
    if (const auto check = expect_equal(
            result.liquidity,
            LiquidityRegime::Healthy,
            "liquidity regime"
        );
        check != 0) {
        return check;
    }
    if (const auto check =
            expect_equal(result.risk, RiskRegime::Healthy, "risk regime");
        check != 0) {
        return check;
    }
    return expect_equal(
        result.execution,
        ExecutionRegime::Healthy,
        "execution regime"
    );
}

int test_stale_when_book_stale() {
    const auto view = healthy_depth();
    RegimeInput input;
    input.now_ns = 2'000'000'001ULL;
    input.depth_views = std::span<const MarketDepthView>{&view, 1};

    const auto result = RegimeEngine{}.classify(input);
    return expect_equal(result.data, DataRegime::Stale, "data regime");
}

int test_risk_rejecting() {
    const std::array<RiskDecision, 2> decisions{
        rejected_decision(),
        rejected_decision()
    };
    const auto view = healthy_depth();

    RegimeInput input;
    input.now_ns = 1'100;
    input.depth_views = std::span<const MarketDepthView>{&view, 1};
    input.risk_decisions = decisions;

    const auto result = RegimeEngine{}.classify(input);
    return expect_equal(result.risk, RiskRegime::Constrained, "risk regime");
}

int test_hedge_required() {
    ExecutionReport report;
    report.status = ChildOrderStatus::PartiallyFilled;
    const auto view = healthy_depth();

    RegimeInput input;
    input.now_ns = 1'100;
    input.depth_views = std::span<const MarketDepthView>{&view, 1};
    input.execution_reports = std::span<const ExecutionReport>{&report, 1};

    const auto result = RegimeEngine{}.classify(input);
    return expect_equal(
        result.execution,
        ExecutionRegime::HedgeRequired,
        "execution regime"
    );
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return fail("expected one test case name");
    }

    const std::string test_case{argv[1]};
    if (test_case == "Regime_HealthyWhenAllOk") {
        return test_healthy_when_all_ok();
    }
    if (test_case == "Regime_StaleWhenBookStale") {
        return test_stale_when_book_stale();
    }
    if (test_case == "Regime_RiskRejecting") {
        return test_risk_rejecting();
    }
    if (test_case == "Regime_HedgeRequired") {
        return test_hedge_required();
    }

    return fail("unknown test case");
}
