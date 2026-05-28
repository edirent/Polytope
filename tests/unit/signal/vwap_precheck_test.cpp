#include "engine/signal/pricing/VWAPPrecheck.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::oracle::BundleLeg;
using trading_engine::oracle::CandidateBundle;
using trading_engine::oracle::Side;
using trading_engine::signal::CostFailureReason;
using trading_engine::signal::VWAPPrecheck;
using trading_engine::state::MarketStateSnapshot;
using trading_engine::state::PriceLevel;

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

MarketStateSnapshot snapshot_with_asks(
    std::initializer_list<PriceLevel> asks
) {
    MarketStateSnapshot snapshot;
    snapshot.entity_id = "asset_yes";
    snapshot.market_id = "m1";
    snapshot.live = true;
    snapshot.usable_for_depth = true;
    snapshot.usable_for_signal = true;

    std::uint32_t index = 0;
    for (const auto& ask : asks) {
        snapshot.asks[index++] = ask;
    }
    snapshot.ask_count = index;
    snapshot.has_ask = index > 0;
    if (index > 0) {
        snapshot.best_ask_tick = snapshot.asks[0].price_tick;
    }
    return snapshot;
}

CandidateBundle buy_bundle(std::int64_t quantity_lots) {
    CandidateBundle bundle;
    bundle.bundle_id = 1;
    bundle.leg_count = 1;
    bundle.legs[0] = BundleLeg{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .side = Side::Buy,
        .quantity_lots = quantity_lots,
        .max_price_tick = 1'000'000
    };
    return bundle;
}

void VWAPPrecheck_BuyConsumesAsks() {
    const auto snapshot = snapshot_with_asks({
        PriceLevel{.price_tick = 500'000, .price = 0.50, .size = 10.0}
    });

    const auto result = VWAPPrecheck{}.price_bundle(
        buy_bundle(5),
        {snapshot}
    );

    expect_true(result.enough_depth, "enough_depth");
    expect_equal(result.failure_reason, CostFailureReason::None, "failure");
    expect_equal(result.total_cost_tick, 2'500'000LL, "total cost");
    expect_equal(result.bundle_vwap_tick, 500'000LL, "bundle vwap");
    expect_equal(result.worst_price_tick, 500'000LL, "worst price");
    expect_equal(result.filled_leg_count, static_cast<std::uint16_t>(1), "legs");
    expect_true(result.priced_legs[0].enough_depth, "leg depth");
    expect_equal(
        result.priced_legs[0].estimated_cost_tick,
        2'500'000LL,
        "leg cost"
    );
}

void VWAPPrecheck_MultiLevelBuyComputesVWAP() {
    const auto snapshot = snapshot_with_asks({
        PriceLevel{.price_tick = 500'000, .price = 0.50, .size = 10.0},
        PriceLevel{.price_tick = 550'000, .price = 0.55, .size = 10.0}
    });

    const auto result = VWAPPrecheck{}.price_bundle(
        buy_bundle(15),
        {snapshot}
    );

    expect_true(result.enough_depth, "enough_depth");
    expect_equal(result.total_cost_tick, 7'750'000LL, "total cost");
    expect_equal(result.bundle_vwap_tick, 516'666LL, "bundle vwap");
    expect_equal(result.worst_price_tick, 550'000LL, "worst price");
    expect_equal(
        result.priced_legs[0].estimated_vwap_tick,
        516'666LL,
        "leg vwap"
    );
}

void VWAPPrecheck_RejectsInsufficientDepth() {
    const auto snapshot = snapshot_with_asks({
        PriceLevel{.price_tick = 500'000, .price = 0.50, .size = 10.0},
        PriceLevel{.price_tick = 550'000, .price = 0.55, .size = 10.0}
    });

    const auto result = VWAPPrecheck{}.price_bundle(
        buy_bundle(25),
        {snapshot}
    );

    expect_false(result.enough_depth, "enough_depth");
    expect_equal(
        result.failure_reason,
        CostFailureReason::InsufficientDepth,
        "failure"
    );
    expect_equal(result.failed_leg_index, static_cast<std::uint16_t>(0), "leg");
}

void VWAPPrecheck_RejectsMissingAskSide() {
    MarketStateSnapshot snapshot;
    snapshot.entity_id = "asset_yes";
    snapshot.market_id = "m1";
    snapshot.has_ask = false;
    snapshot.ask_count = 0;

    const auto result = VWAPPrecheck{}.price_bundle(
        buy_bundle(1),
        {snapshot}
    );

    expect_false(result.enough_depth, "enough_depth");
    expect_equal(
        result.failure_reason,
        CostFailureReason::MissingBookSide,
        "failure"
    );
}

void VWAPPrecheck_RejectsInvalidQuantity() {
    const auto snapshot = snapshot_with_asks({
        PriceLevel{.price_tick = 500'000, .price = 0.50, .size = 10.0}
    });

    const auto result = VWAPPrecheck{}.price_bundle(
        buy_bundle(0),
        {snapshot}
    );

    expect_false(result.enough_depth, "enough_depth");
    expect_equal(
        result.failure_reason,
        CostFailureReason::InvalidQuantity,
        "failure"
    );
}

void VWAPPrecheck_RejectsUnsupportedSellLegInV0() {
    const auto snapshot = snapshot_with_asks({
        PriceLevel{.price_tick = 500'000, .price = 0.50, .size = 10.0}
    });
    auto bundle = buy_bundle(1);
    bundle.legs[0].side = Side::Sell;

    const auto result = VWAPPrecheck{}.price_bundle(bundle, {snapshot});

    expect_false(result.enough_depth, "enough_depth");
    expect_equal(
        result.failure_reason,
        CostFailureReason::InvalidLeg,
        "failure"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"VWAPPrecheck_BuyConsumesAsks", &VWAPPrecheck_BuyConsumesAsks},
        {
            "VWAPPrecheck_MultiLevelBuyComputesVWAP",
            &VWAPPrecheck_MultiLevelBuyComputesVWAP
        },
        {
            "VWAPPrecheck_RejectsInsufficientDepth",
            &VWAPPrecheck_RejectsInsufficientDepth
        },
        {
            "VWAPPrecheck_RejectsMissingAskSide",
            &VWAPPrecheck_RejectsMissingAskSide
        },
        {
            "VWAPPrecheck_RejectsInvalidQuantity",
            &VWAPPrecheck_RejectsInvalidQuantity
        },
        {
            "VWAPPrecheck_RejectsUnsupportedSellLegInV0",
            &VWAPPrecheck_RejectsUnsupportedSellLegInV0
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
