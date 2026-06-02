#include "engine/strategy/market_making/core/MarketMakingEngine.h"
#include "engine/strategy/market_making/tools/MarketMakingTools.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using namespace trading_engine::strategy::market_making;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

template <typename Actual, typename Expected>
void expect_equal(const Actual& actual, const Expected& expected, const std::string& field) {
    if (!(actual == expected)) {
        fail("mismatch: " + field);
    }
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

MarketMakingConfig config() {
    MarketMakingConfig cfg;
    cfg.strategy_id = 3;
    cfg.min_half_spread_tick = 5'000;
    cfg.base_quote_size_lots = 5;
    cfg.max_inventory_lots = 100;
    cfg.requote_threshold_tick = 2'000;
    cfg.inventory_requote_threshold_lots = 5;
    cfg.max_inventory_skew_tick = 10'000;
    return cfg;
}

MarketMakingInput input(const trading_engine::state::MarketDepthView& depth, std::int64_t pos, std::uint64_t now = 100) {
    return MarketMakingInput{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .market_index = 1,
        .asset_index = 7,
        .depth = &depth,
        .current_position_lots = pos,
        .now_ns = now
    };
}

void MarketMakingWorkflow_PostsQuote() {
    MarketMakingEngine engine(config());
    auto depth = tools::make_depth_view(490'000, 510'000, 20.0, 20.0, 1, 100);
    const auto result = engine.on_market_update(input(depth, 20));
    expect_equal(result.quotes_emitted, 1ULL, "quotes");
    expect_equal(result.cancels_emitted, 0ULL, "cancels");
    expect_true(engine.quote_book().find(7) != nullptr, "active");
}

void MarketMakingWorkflow_CancelsOnFairMove() {
    MarketMakingEngine engine(config());
    auto depth1 = tools::make_depth_view(490'000, 510'000, 20.0, 20.0, 1, 100);
    auto depth2 = tools::make_depth_view(520'000, 540'000, 20.0, 20.0, 2, 200);
    (void)engine.on_market_update(input(depth1, 20, 100));
    const auto result = engine.on_market_update(input(depth2, 20, 200));
    expect_equal(result.cancels_emitted, 1ULL, "cancels");
    expect_equal(result.replacements, 1ULL, "replace");
    expect_equal(result.quotes_emitted, 1ULL, "quotes");
}

void MarketMakingWorkflow_InventorySkewChangesQuote() {
    MarketMakingEngine engine(config());
    auto depth = tools::make_depth_view(490'000, 510'000, 20.0, 20.0, 1, 100);
    const auto result = engine.on_market_update(input(depth, 50, 100));
    expect_equal(result.quotes_emitted, 1ULL, "quotes");
    expect_true(result.quotes[0].inventory_skew_tick > 0, "skew");
    expect_true(result.quotes[0].bid.price_tick < 495'000, "bid shifted down");
}

void MarketMakingWorkflow_CancelsOnBadBook() {
    MarketMakingEngine engine(config());
    auto depth = tools::make_depth_view(490'000, 510'000, 20.0, 20.0, 1, 100);
    (void)engine.on_market_update(input(depth, 20, 100));
    depth.usable_for_depth = false;
    const auto result = engine.on_market_update(input(depth, 20, 200));
    expect_equal(result.cancels_emitted, 1ULL, "cancels");
    expect_true(engine.quote_book().find(7) == nullptr, "removed");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> map{
        {"MarketMakingWorkflow_PostsQuote", &MarketMakingWorkflow_PostsQuote},
        {"MarketMakingWorkflow_CancelsOnFairMove", &MarketMakingWorkflow_CancelsOnFairMove},
        {"MarketMakingWorkflow_InventorySkewChangesQuote", &MarketMakingWorkflow_InventorySkewChangesQuote},
        {"MarketMakingWorkflow_CancelsOnBadBook", &MarketMakingWorkflow_CancelsOnBadBook}
    };
    return map;
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
