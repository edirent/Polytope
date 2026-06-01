#include "engine/common/math/RiskMath.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::common::math::RiskEdgeMathInput;
using trading_engine::common::math::compute_post_risk_edge;
using trading_engine::common::math::passes_edge_thresholds;
using trading_engine::risk::RiskPolicySnapshot;

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

RiskPolicySnapshot policy() {
    RiskPolicySnapshot out;
    out.min_post_risk_total_edge_tick = 250'000;
    out.min_post_risk_unit_edge_tick = 80'000;
    out.min_edge_bps = 1'000;
    return out;
}

void RiskMath_ComputesPostRiskEdge() {
    const auto result = compute_post_risk_edge(RiskEdgeMathInput{
        .guaranteed_payout_per_bundle_tick = 1'000'000,
        .risk_bundle_qty = 3,
        .risk_total_cost_tick = 2'700'000,
        .fee_tick = 10'000,
        .slippage_buffer_tick = 5'000,
        .latency_buffer_tick = 0
    });

    expect_true(result.ok, "ok");
    expect_equal(result.post_risk_edge_tick, 285'000LL, "total edge");
    expect_equal(result.unit_edge_tick, 95'000LL, "unit edge");
    expect_equal(result.edge_bps, 1'055LL, "edge bps");
}

void RiskMath_PassesEdgeThresholds() {
    expect_true(
        passes_edge_thresholds(95'000, 285'000, 1'055, policy()),
        "thresholds"
    );
}

void RiskMath_RejectsLowUnitEdge() {
    expect_false(
        passes_edge_thresholds(79'999, 285'000, 1'055, policy()),
        "thresholds"
    );
}

void RiskMath_RejectsLowTotalEdge() {
    expect_false(
        passes_edge_thresholds(95'000, 249'999, 1'055, policy()),
        "thresholds"
    );
}

void RiskMath_RejectsLowBps() {
    expect_false(
        passes_edge_thresholds(95'000, 285'000, 999, policy()),
        "thresholds"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"RiskMath_ComputesPostRiskEdge", &RiskMath_ComputesPostRiskEdge},
        {"RiskMath_PassesEdgeThresholds", &RiskMath_PassesEdgeThresholds},
        {"RiskMath_RejectsLowUnitEdge", &RiskMath_RejectsLowUnitEdge},
        {"RiskMath_RejectsLowTotalEdge", &RiskMath_RejectsLowTotalEdge},
        {"RiskMath_RejectsLowBps", &RiskMath_RejectsLowBps},
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
