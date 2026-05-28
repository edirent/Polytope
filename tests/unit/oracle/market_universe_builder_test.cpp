#include "oracle/ingestion/MarketUniverseBuilder.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::oracle::MarketUniverseBuilder;
using trading_engine::oracle::RawMarketRecord;

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

RawMarketRecord market(
    std::string market_id,
    std::string yes_asset,
    std::string no_asset
) {
    RawMarketRecord record;
    record.market_id = std::move(market_id);
    record.event_id = "event";
    record.title = "Winner?";
    record.description = "fixture";
    record.outcomes = {"YES", "NO"};
    record.asset_ids = {std::move(yes_asset), std::move(no_asset)};
    record.source = "fixture";
    return record;
}

void MarketUniverseBuilder_DeduplicatesMarkets() {
    const std::vector<RawMarketRecord> records{
        market("m1", "asset_yes", "asset_no"),
        market("m1", "asset_yes_duplicate", "asset_no_duplicate")
    };

    MarketUniverseBuilder builder;
    const auto result = builder.build(records);

    expect_true(result.ok(), "build ok");
    expect_equal(result.universe.markets.size(), 1U, "market count");
    expect_equal(result.universe.manifest.market_count, 1U, "manifest markets");
    expect_equal(result.universe.manifest.asset_count, 2U, "manifest assets");
    expect_false(result.warnings.empty(), "duplicate warning");
}

void MarketUniverseBuilder_PreservesAssetIdsAsString() {
    const std::string huge_asset =
        "113331598718835447619835372415650100713271301516293755503999990681415131593110";
    const std::vector<RawMarketRecord> records{
        market("m1", huge_asset, "asset_no")
    };

    MarketUniverseBuilder builder;
    const auto result = builder.build(records);

    expect_true(result.ok(), "build ok");
    expect_equal(
        result.universe.markets.front().asset_ids.front(),
        huge_asset,
        "asset id string"
    );
    expect_equal(result.universe.manifest.asset_count, 2U, "asset count");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "MarketUniverseBuilder_DeduplicatesMarkets",
            &MarketUniverseBuilder_DeduplicatesMarkets
        },
        {
            "MarketUniverseBuilder_PreservesAssetIdsAsString",
            &MarketUniverseBuilder_PreservesAssetIdsAsString
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
