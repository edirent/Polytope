#include "engine/signal/pricing/PriceVectorBuilder.h"

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
using trading_engine::signal::ExecutableBookSide;
using trading_engine::signal::PriceVectorBuilder;
using trading_engine::state::MarketStateSnapshot;

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

MarketStateSnapshot snapshot(const std::string& asset_id) {
    MarketStateSnapshot out;
    out.entity_id = asset_id;
    out.market_id = "m1";
    out.version = 1;
    out.usable_for_depth = true;
    return out;
}

CandidateBundle bundle(Side side, std::int64_t quantity_lots = 10) {
    CandidateBundle out;
    out.bundle_id = 1;
    out.leg_count = 1;
    out.legs[0] = BundleLeg{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .side = side,
        .quantity_lots = quantity_lots,
        .max_price_tick = 1'000'000
    };
    return out;
}

void PriceVectorBuilder_BuyLegUsesAsks() {
    const auto result = PriceVectorBuilder{}.build(
        bundle(Side::Buy),
        {snapshot("asset_yes")}
    );

    expect_true(result.ok, "result ok");
    expect_equal(result.failure_reason, CostFailureReason::None, "failure");
    expect_equal(result.leg_count, static_cast<std::uint16_t>(1), "leg count");
    expect_equal(
        result.legs[0].executable_side,
        ExecutableBookSide::Asks,
        "book side"
    );
    expect_equal(result.legs[0].target_qty_lots, 10LL, "quantity");
    expect_true(result.legs[0].snapshot != nullptr, "snapshot pointer");
}

void PriceVectorBuilder_SellLegUnsupportedInV0() {
    const auto result = PriceVectorBuilder{}.build(
        bundle(Side::Sell),
        {snapshot("asset_yes")}
    );

    expect_false(result.ok, "result ok");
    expect_equal(
        result.failure_reason,
        CostFailureReason::InvalidLeg,
        "failure"
    );
    expect_equal(result.failed_leg_index, static_cast<std::uint16_t>(0), "leg");
}

void PriceVectorBuilder_RejectsMissingSnapshot() {
    const auto result = PriceVectorBuilder{}.build(
        bundle(Side::Buy),
        {snapshot("asset_no")}
    );

    expect_false(result.ok, "result ok");
    expect_equal(
        result.failure_reason,
        CostFailureReason::MissingSnapshot,
        "failure"
    );
    expect_equal(result.failed_leg_index, static_cast<std::uint16_t>(0), "leg");
}

void PriceVectorBuilder_RejectsInvalidQuantity() {
    const auto result = PriceVectorBuilder{}.build(
        bundle(Side::Buy, 0),
        {snapshot("asset_yes")}
    );

    expect_false(result.ok, "result ok");
    expect_equal(
        result.failure_reason,
        CostFailureReason::InvalidQuantity,
        "failure"
    );
    expect_equal(result.failed_leg_index, static_cast<std::uint16_t>(0), "leg");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"PriceVectorBuilder_BuyLegUsesAsks", &PriceVectorBuilder_BuyLegUsesAsks},
        {
            "PriceVectorBuilder_SellLegUnsupportedInV0",
            &PriceVectorBuilder_SellLegUnsupportedInV0
        },
        {
            "PriceVectorBuilder_RejectsMissingSnapshot",
            &PriceVectorBuilder_RejectsMissingSnapshot
        },
        {
            "PriceVectorBuilder_RejectsInvalidQuantity",
            &PriceVectorBuilder_RejectsInvalidQuantity
        }
    };
    return test_map;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <test-name>\n";
        return 2;
    }

    const std::string name = argv[1];
    const auto it = tests().find(name);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << name << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& ex) {
        std::cerr << name << " failed: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
