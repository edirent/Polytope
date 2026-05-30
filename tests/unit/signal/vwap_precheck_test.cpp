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

    expect_true(result.executable, "executable");
    expect_equal(result.failure_reason, CostFailureReason::None, "failure");
    expect_equal(result.bundle_qty, 2LL, "bundle qty");
    expect_equal(result.total_cost_tick, 5'000'000LL, "total cost");
    expect_equal(result.avg_cost_tick, 2'500'000LL, "avg cost");
    expect_equal(result.legs.size(), 1U, "legs");
    expect_true(result.legs[0].enough_depth, "leg depth");
    expect_equal(
        result.legs[0].executable_qty_lots,
        10LL,
        "executable qty"
    );
    expect_equal(
        result.legs[0].total_cost_tick,
        5'000'000LL,
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

    expect_true(result.executable, "executable");
    expect_equal(result.bundle_qty, 1LL, "bundle qty");
    expect_equal(result.total_cost_tick, 7'750'000LL, "total cost");
    expect_equal(result.avg_cost_tick, 7'750'000LL, "avg cost");
    expect_equal(result.max_leg_slippage_tick, 50'000LL, "slippage");
    expect_equal(
        result.legs[0].vwap_price_tick,
        516'666LL,
        "leg vwap"
    );
    expect_equal(result.legs[0].worst_price_tick, 550'000LL, "worst");
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

    expect_false(result.executable, "executable");
    expect_equal(
        result.failure_reason,
        CostFailureReason::InsufficientDepth,
        "failure"
    );
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

    expect_false(result.executable, "executable");
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

    expect_false(result.executable, "executable");
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

    expect_false(result.executable, "executable");
    expect_equal(
        result.failure_reason,
        CostFailureReason::InvalidLeg,
        "failure"
    );
}

void VWAPPrecheck_ComputesExecutableQtyPerLeg() {
    const auto snapshot = snapshot_with_asks({
        PriceLevel{.price_tick = 500'000, .price = 0.50, .size = 7.0}
    });

    const auto result = VWAPPrecheck{}.price_bundle(
        buy_bundle(2),
        {snapshot}
    );

    expect_true(result.executable, "executable");
    expect_equal(result.legs[0].requested_qty_lots, 2LL, "requested");
    expect_equal(result.legs[0].executable_qty_lots, 7LL, "executable qty");
    expect_equal(result.bundle_qty, 3LL, "bundle qty");
}

CandidateBundle two_leg_bundle(
    std::int64_t first_qty,
    std::int64_t second_qty
) {
    CandidateBundle bundle;
    bundle.bundle_id = 2;
    bundle.leg_count = 2;
    bundle.legs[0] = BundleLeg{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .side = Side::Buy,
        .quantity_lots = first_qty,
        .max_price_tick = 1'000'000
    };
    bundle.legs[1] = BundleLeg{
        .market_id = "m1",
        .asset_id = "asset_no",
        .side = Side::Buy,
        .quantity_lots = second_qty,
        .max_price_tick = 1'000'000
    };
    return bundle;
}

MarketStateSnapshot snapshot_for_asset(
    const std::string& asset_id,
    std::initializer_list<PriceLevel> asks
) {
    auto snapshot = snapshot_with_asks(asks);
    snapshot.entity_id = asset_id;
    return snapshot;
}

void VWAPPrecheck_BundleQtyIsMinAcrossLegs() {
    const auto result = VWAPPrecheck{}.price_bundle(
        two_leg_bundle(1, 1),
        {
            snapshot_for_asset(
                "asset_yes",
                {PriceLevel{.price_tick = 400'000, .price = 0.40, .size = 10.0}}
            ),
            snapshot_for_asset(
                "asset_no",
                {PriceLevel{.price_tick = 450'000, .price = 0.45, .size = 3.0}}
            )
        }
    );

    expect_true(result.executable, "executable");
    expect_equal(result.bundle_qty, 3LL, "bundle qty");
}

void VWAPPrecheck_TotalCostUsesBundleQty() {
    const auto result = VWAPPrecheck{}.price_bundle(
        two_leg_bundle(1, 1),
        {
            snapshot_for_asset(
                "asset_yes",
                {PriceLevel{.price_tick = 400'000, .price = 0.40, .size = 10.0}}
            ),
            snapshot_for_asset(
                "asset_no",
                {PriceLevel{.price_tick = 450'000, .price = 0.45, .size = 3.0}}
            )
        }
    );

    expect_true(result.executable, "executable");
    expect_equal(result.bundle_qty, 3LL, "bundle qty");
    expect_equal(result.total_cost_tick, 2'550'000LL, "total cost");
    expect_equal(result.avg_cost_tick, 850'000LL, "avg cost");
}

void VWAPPrecheck_RejectsIfAnyLegInsufficient() {
    const auto result = VWAPPrecheck{}.price_bundle(
        two_leg_bundle(5, 5),
        {
            snapshot_for_asset(
                "asset_yes",
                {PriceLevel{.price_tick = 400'000, .price = 0.40, .size = 5.0}}
            ),
            snapshot_for_asset(
                "asset_no",
                {PriceLevel{.price_tick = 450'000, .price = 0.45, .size = 4.0}}
            )
        }
    );

    expect_false(result.executable, "executable");
    expect_equal(
        result.failure_reason,
        CostFailureReason::InsufficientDepth,
        "failure"
    );
}

void VWAPPrecheck_ComputesWorstPrice() {
    const auto result = VWAPPrecheck{}.price_bundle(
        buy_bundle(3),
        {
            snapshot_with_asks({
                PriceLevel{.price_tick = 500'000, .price = 0.50, .size = 3.0},
                PriceLevel{.price_tick = 550'000, .price = 0.55, .size = 3.0}
            })
        }
    );

    expect_true(result.executable, "executable");
    expect_equal(result.bundle_qty, 2LL, "bundle qty");
    expect_equal(result.legs[0].worst_price_tick, 550'000LL, "worst");
    expect_equal(result.max_leg_slippage_tick, 50'000LL, "slippage");
}

void VWAPPrecheck_ComputesAvgCost() {
    const auto result = VWAPPrecheck{}.price_bundle(
        buy_bundle(2),
        {
            snapshot_with_asks({
                PriceLevel{.price_tick = 500'000, .price = 0.50, .size = 4.0}
            })
        }
    );

    expect_true(result.executable, "executable");
    expect_equal(result.bundle_qty, 2LL, "bundle qty");
    expect_equal(result.total_cost_tick, 2'000'000LL, "total cost");
    expect_equal(result.avg_cost_tick, 1'000'000LL, "avg cost");
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
        },
        {
            "VWAPPrecheck_ComputesExecutableQtyPerLeg",
            &VWAPPrecheck_ComputesExecutableQtyPerLeg
        },
        {
            "VWAPPrecheck_BundleQtyIsMinAcrossLegs",
            &VWAPPrecheck_BundleQtyIsMinAcrossLegs
        },
        {
            "VWAPPrecheck_TotalCostUsesBundleQty",
            &VWAPPrecheck_TotalCostUsesBundleQty
        },
        {
            "VWAPPrecheck_RejectsIfAnyLegInsufficient",
            &VWAPPrecheck_RejectsIfAnyLegInsufficient
        },
        {
            "VWAPPrecheck_ComputesWorstPrice",
            &VWAPPrecheck_ComputesWorstPrice
        },
        {
            "VWAPPrecheck_ComputesAvgCost",
            &VWAPPrecheck_ComputesAvgCost
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
