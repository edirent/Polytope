#include "state/book/DepthPrefix.h"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::state::DepthPrefix;
using trading_engine::state::PrefixVwapResult;
using trading_engine::state::PriceLevel;
using trading_engine::state::build_depth_prefix;
using trading_engine::state::buy_vwap_from_prefix;
using trading_engine::state::kMaxSnapshotDepth;

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

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

std::array<PriceLevel, kMaxSnapshotDepth> asks() {
    std::array<PriceLevel, kMaxSnapshotDepth> levels{};
    levels[0] = PriceLevel{.price_tick = 500'000, .price = 0.50, .size = 10.0};
    levels[1] = PriceLevel{.price_tick = 550'000, .price = 0.55, .size = 5.0};
    return levels;
}

DepthPrefix prefix_for_asks(
    const std::array<PriceLevel, kMaxSnapshotDepth>& ask_levels,
    std::uint16_t ask_count
) {
    DepthPrefix prefix;
    std::array<PriceLevel, kMaxSnapshotDepth> bids{};
    build_depth_prefix(bids, 0, ask_levels, ask_count, &prefix);
    return prefix;
}

void DepthPrefix_BuildsAskCumulativeQty() {
    const auto ask_levels = asks();
    const auto prefix = prefix_for_asks(ask_levels, 2);

    expect_equal(prefix.ask_count, static_cast<std::uint16_t>(2), "count");
    expect_equal(prefix.ask_cum_qty[0], 10LL, "first qty");
    expect_equal(prefix.ask_cum_qty[1], 15LL, "second qty");
}

void DepthPrefix_BuildsAskCumulativeCost() {
    const auto ask_levels = asks();
    const auto prefix = prefix_for_asks(ask_levels, 2);

    expect_equal(prefix.ask_cum_cost[0], 5'000'000LL, "first cost");
    expect_equal(prefix.ask_cum_cost[1], 7'750'000LL, "second cost");
}

void DepthPrefix_BuyVwapExactFirstLevel() {
    const auto ask_levels = asks();
    const auto prefix = prefix_for_asks(ask_levels, 2);

    const PrefixVwapResult result =
        buy_vwap_from_prefix(ask_levels, prefix, 10);

    expect_true(result.ok, "ok");
    expect_equal(result.total_cost_tick, 5'000'000LL, "cost");
    expect_equal(result.vwap_tick, 500'000LL, "vwap");
    expect_equal(result.worst_price_tick, 500'000LL, "worst");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"DepthPrefix_BuildsAskCumulativeQty",
         &DepthPrefix_BuildsAskCumulativeQty},
        {"DepthPrefix_BuildsAskCumulativeCost",
         &DepthPrefix_BuildsAskCumulativeCost},
        {"DepthPrefix_BuyVwapExactFirstLevel",
         &DepthPrefix_BuyVwapExactFirstLevel},
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
