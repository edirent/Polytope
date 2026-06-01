#include "engine/order_decision/sizing/SizeCandidateEnumerator.h"

#include <exception>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using trading_engine::order_decision::CostCurve;
using trading_engine::order_decision::SizeCandidateEnumerationInput;
using trading_engine::order_decision::SizeCandidateEnumerator;
using trading_engine::oracle::Side;

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

CostCurve curve(std::initializer_list<std::pair<std::int64_t, std::int64_t>> levels) {
    CostCurve out;
    out.side = Side::Buy;
    for (const auto& [qty, cost] : levels) {
        auto& level = out.levels[out.level_count++];
        level.price_tick = 100'000;
        level.cumulative_qty_lots = qty;
        level.cumulative_cost_tick = cost;
        level.size_lots = qty - (out.level_count > 1
            ? out.levels[out.level_count - 2].cumulative_qty_lots
            : 0);
        out.total_qty_lots = qty;
    }
    return out;
}

void SizeEnumerator_GeneratesBreakpoints() {
    const std::array curves{curve({{5, 500'000}, {10, 1'000'000}})};
    std::array<std::int64_t, 16> ratios{};
    ratios[0] = 1;

    const auto result = SizeCandidateEnumerator{}.enumerate({
        .curves = std::span<const CostCurve>(curves.data(), 1),
        .ratio_qty_lots = ratios,
        .leg_count = 1,
        .min_bundle_qty = 1
    });

    expect_equal(result.candidates.size(), 2U, "candidate count");
    expect_equal(result.candidates[0], 5LL, "first");
    expect_equal(result.candidates[1], 10LL, "second");
}

void SizeEnumerator_DeduplicatesCandidates() {
    const std::array curves{
        curve({{5, 500'000}, {10, 1'000'000}}),
        curve({{5, 500'000}, {10, 1'000'000}})
    };
    std::array<std::int64_t, 16> ratios{};
    ratios[0] = 1;
    ratios[1] = 1;

    const auto result = SizeCandidateEnumerator{}.enumerate({
        .curves = std::span<const CostCurve>(curves.data(), 2),
        .ratio_qty_lots = ratios,
        .leg_count = 2,
        .min_bundle_qty = 1
    });

    expect_equal(result.candidates.size(), 2U, "candidate count");
    expect_equal(result.candidates[0], 5LL, "first");
    expect_equal(result.candidates[1], 10LL, "second");
}

void SizeEnumerator_ClampsToRiskBudget() {
    const std::array curves{curve({{5, 500'000}, {10, 2'000'000}})};
    std::array<std::int64_t, 16> ratios{};
    ratios[0] = 1;

    const auto result = SizeCandidateEnumerator{}.enumerate({
        .curves = std::span<const CostCurve>(curves.data(), 1),
        .ratio_qty_lots = ratios,
        .leg_count = 1,
        .min_bundle_qty = 1,
        .max_bundle_qty = 0,
        .max_total_cost_tick = 900'000
    });

    expect_equal(result.candidates.size(), 1U, "candidate count");
    expect_equal(result.candidates[0], 5LL, "first");
}

std::vector<std::int64_t> old_generate_sort_dedup(
    const SizeCandidateEnumerationInput& input
) {
    std::vector<std::int64_t> out;
    for (std::uint16_t i = 0; i < input.leg_count; ++i) {
        const auto ratio = input.ratio_qty_lots[i];
        if (ratio <= 0) {
            continue;
        }
        const auto& c = input.curves[i];
        for (std::uint16_t level = 0; level < c.level_count; ++level) {
            auto q = c.levels[level].cumulative_qty_lots / ratio;
            if (q <= 0 || q < input.min_bundle_qty) {
                continue;
            }
            if (input.max_bundle_qty > 0 && q > input.max_bundle_qty) {
                q = input.max_bundle_qty;
            }
            if (q >= input.min_bundle_qty) {
                out.push_back(q);
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void KWayCandidateEnumerator_ProducesSameCandidatesAsGeneric() {
    const std::array curves{
        curve({{3, 300'000}, {7, 700'000}, {11, 1'100'000}}),
        curve({{4, 400'000}, {10, 1'000'000}, {15, 1'500'000}}),
        curve({{2, 200'000}, {9, 900'000}, {20, 2'000'000}})
    };
    std::array<std::int64_t, 16> ratios{};
    ratios[0] = 1;
    ratios[1] = 2;
    ratios[2] = 3;

    const SizeCandidateEnumerationInput input{
        .curves = std::span<const CostCurve>(curves.data(), curves.size()),
        .ratio_qty_lots = ratios,
        .leg_count = static_cast<std::uint16_t>(curves.size()),
        .min_bundle_qty = 1,
        .max_bundle_qty = 8
    };

    const auto expected = old_generate_sort_dedup(input);
    const auto actual = SizeCandidateEnumerator{}.enumerate(input);

    expect_equal(actual.candidates.size(), expected.size(), "candidate count");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        expect_equal(actual.candidates[i], expected[i], "candidate");
    }
    expect_equal(actual.candidate_sort_dedup_ns, 0ULL, "sort dedup skipped");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"SizeEnumerator_GeneratesBreakpoints", &SizeEnumerator_GeneratesBreakpoints},
        {"SizeEnumerator_DeduplicatesCandidates", &SizeEnumerator_DeduplicatesCandidates},
        {"SizeEnumerator_ClampsToRiskBudget", &SizeEnumerator_ClampsToRiskBudget},
        {
            "KWayCandidateEnumerator_ProducesSameCandidatesAsGeneric",
            &KWayCandidateEnumerator_ProducesSameCandidatesAsGeneric
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
    if (argc == 2) {
        return run_test(argv[1]);
    }
    int failures = 0;
    for (const auto& [name, _] : tests()) {
        failures += run_test(name) == 0 ? 0 : 1;
    }
    return failures == 0 ? 0 : 1;
}
