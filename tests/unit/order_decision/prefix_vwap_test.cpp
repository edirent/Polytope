#include "engine/common/math/VwapMath.h"
#include "engine/order_decision/math/PrefixVwap.h"
#include "state/book/DepthPrefix.h"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::state::MarketDepthView;
using trading_engine::state::PriceLevel;
using trading_engine::state::build_depth_prefix;
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

void expect_false(bool value, const std::string& field) {
    if (value) {
        fail("expected false: " + field);
    }
}

MarketDepthView depth() {
    MarketDepthView view;
    view.usable_for_depth = true;
    view.ask_count = 2;
    view.asks[0] = PriceLevel{
        .price_tick = 500'000,
        .price = 0.50,
        .size = 10.0
    };
    view.asks[1] = PriceLevel{
        .price_tick = 550'000,
        .price = 0.55,
        .size = 5.0
    };
    build_depth_prefix(
        view.bids,
        view.bid_count,
        view.asks,
        view.ask_count,
        &view.prefix
    );
    return view;
}

void PrefixVWAP_OneLevelExact() {
    const auto view = depth();
    const auto result =
        trading_engine::order_decision::buy_vwap_from_prefix(view, 10);

    expect_true(result.ok, "ok");
    expect_equal(result.total_cost_tick, 5'000'000LL, "cost");
    expect_equal(result.vwap_tick, 500'000LL, "vwap");
    expect_equal(result.worst_price_tick, 500'000LL, "worst");
    expect_equal(result.level_index, static_cast<std::uint16_t>(0), "level");
}

void PrefixVWAP_MultiLevelExact() {
    const auto view = depth();
    const auto result =
        trading_engine::order_decision::buy_vwap_from_prefix(view, 12);

    expect_true(result.ok, "ok");
    expect_equal(result.total_cost_tick, 6'100'000LL, "cost");
    expect_equal(result.vwap_tick, 508'333LL, "vwap");
    expect_equal(result.worst_price_tick, 550'000LL, "worst");
    expect_equal(result.level_index, static_cast<std::uint16_t>(1), "level");
}

void PrefixVWAP_ExactBoundary() {
    const auto view = depth();
    const auto result =
        trading_engine::order_decision::buy_vwap_from_prefix(view, 10);

    expect_true(result.ok, "ok");
    expect_equal(result.level_index, static_cast<std::uint16_t>(0), "level");
}

void PrefixVWAP_BoundaryPlusOne() {
    const auto view = depth();
    const auto result =
        trading_engine::order_decision::buy_vwap_from_prefix(view, 11);

    expect_true(result.ok, "ok");
    expect_equal(result.total_cost_tick, 5'550'000LL, "cost");
    expect_equal(result.level_index, static_cast<std::uint16_t>(1), "level");
}

void PrefixVWAP_InsufficientDepth() {
    const auto view = depth();
    const auto result =
        trading_engine::order_decision::buy_vwap_from_prefix(view, 16);

    expect_false(result.ok, "ok");
}

void PrefixVWAP_MatchesLinearVWAP() {
    const auto view = depth();
    for (std::int64_t qty = 1; qty <= 15; ++qty) {
        const auto prefix =
            trading_engine::order_decision::buy_vwap_from_prefix(view, qty);
        const auto linear = trading_engine::common::math::buy_vwap_linear(
            view.asks.data(),
            view.ask_count,
            qty
        );
        expect_equal(prefix.ok, linear.ok, "ok");
        expect_equal(prefix.total_cost_tick, linear.total_cost_tick, "cost");
        expect_equal(prefix.vwap_tick, linear.vwap_tick, "vwap");
        expect_equal(prefix.worst_price_tick, linear.worst_price_tick, "worst");
    }
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"PrefixVWAP_OneLevelExact", &PrefixVWAP_OneLevelExact},
        {"PrefixVWAP_MultiLevelExact", &PrefixVWAP_MultiLevelExact},
        {"PrefixVWAP_ExactBoundary", &PrefixVWAP_ExactBoundary},
        {"PrefixVWAP_BoundaryPlusOne", &PrefixVWAP_BoundaryPlusOne},
        {"PrefixVWAP_InsufficientDepth", &PrefixVWAP_InsufficientDepth},
        {"PrefixVWAP_MatchesLinearVWAP", &PrefixVWAP_MatchesLinearVWAP},
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
