#include "engine/common/math/VwapMath.h"

#include "state/book/DepthPrefix.h"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::common::math::VwapMathResult;
using trading_engine::common::math::buy_vwap_linear;
using trading_engine::common::math::buy_vwap_prefix;
using trading_engine::state::DepthPrefix;
using trading_engine::state::PriceLevel;
using trading_engine::state::build_depth_prefix;
using trading_engine::state::kMaxSnapshotDepth;

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

std::array<PriceLevel, kMaxSnapshotDepth> asks() {
    std::array<PriceLevel, kMaxSnapshotDepth> levels{};
    levels[0] = PriceLevel{.price_tick = 500'000, .price = 0.50, .size = 10.0};
    levels[1] = PriceLevel{.price_tick = 550'000, .price = 0.55, .size = 5.0};
    levels[2] = PriceLevel{.price_tick = 600'000, .price = 0.60, .size = 20.0};
    return levels;
}

DepthPrefix prefix_for(
    const std::array<PriceLevel, kMaxSnapshotDepth>& ask_levels,
    std::uint16_t ask_count
) {
    DepthPrefix prefix;
    std::array<PriceLevel, kMaxSnapshotDepth> bids{};
    build_depth_prefix(bids, 0, ask_levels, ask_count, &prefix);
    return prefix;
}

void expect_same_vwap(
    const VwapMathResult& actual,
    const VwapMathResult& expected
) {
    expect_equal(actual.ok, expected.ok, "ok");
    expect_equal(
        actual.total_cost_tick,
        expected.total_cost_tick,
        "total cost"
    );
    expect_equal(actual.vwap_tick, expected.vwap_tick, "vwap");
    expect_equal(actual.worst_price_tick, expected.worst_price_tick, "worst");
    expect_equal(
        actual.executable_qty_lots,
        expected.executable_qty_lots,
        "executable"
    );
}

void VwapMath_BuyLinearSingleLevel() {
    const auto ask_levels = asks();
    const auto result = buy_vwap_linear(ask_levels.data(), 3, 10);

    expect_true(result.ok, "ok");
    expect_equal(result.total_cost_tick, 5'000'000LL, "cost");
    expect_equal(result.vwap_tick, 500'000LL, "vwap");
    expect_equal(result.worst_price_tick, 500'000LL, "worst");
    expect_equal(result.executable_qty_lots, 35LL, "executable");
}

void VwapMath_BuyLinearMultiLevel() {
    const auto ask_levels = asks();
    const auto result = buy_vwap_linear(ask_levels.data(), 3, 12);

    expect_true(result.ok, "ok");
    expect_equal(result.total_cost_tick, 6'100'000LL, "cost");
    expect_equal(result.vwap_tick, 508'333LL, "vwap");
    expect_equal(result.worst_price_tick, 550'000LL, "worst");
    expect_equal(result.executable_qty_lots, 35LL, "executable");
}

void VwapMath_BuyLinearInsufficientDepthKeepsExecutableQty() {
    const auto ask_levels = asks();
    const auto result = buy_vwap_linear(ask_levels.data(), 2, 20);

    expect_false(result.ok, "ok");
    expect_equal(result.executable_qty_lots, 15LL, "executable");
    expect_equal(result.total_cost_tick, 7'750'000LL, "partial cost");
}

void VwapMath_BuyPrefixMatchesLinear() {
    const auto ask_levels = asks();
    const auto prefix = prefix_for(ask_levels, 3);

    const auto linear = buy_vwap_linear(ask_levels.data(), 3, 21);
    const auto prefix_result =
        buy_vwap_prefix(prefix, ask_levels.data(), 3, 21);

    expect_same_vwap(prefix_result, linear);
}

void VwapMath_BuyPrefixInsufficientDepth() {
    const auto ask_levels = asks();
    const auto prefix = prefix_for(ask_levels, 2);

    const auto result = buy_vwap_prefix(prefix, ask_levels.data(), 2, 20);

    expect_false(result.ok, "ok");
    expect_equal(result.executable_qty_lots, 15LL, "executable");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"VwapMath_BuyLinearSingleLevel", &VwapMath_BuyLinearSingleLevel},
        {"VwapMath_BuyLinearMultiLevel", &VwapMath_BuyLinearMultiLevel},
        {"VwapMath_BuyLinearInsufficientDepthKeepsExecutableQty",
         &VwapMath_BuyLinearInsufficientDepthKeepsExecutableQty},
        {"VwapMath_BuyPrefixMatchesLinear", &VwapMath_BuyPrefixMatchesLinear},
        {"VwapMath_BuyPrefixInsufficientDepth",
         &VwapMath_BuyPrefixInsufficientDepth},
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
