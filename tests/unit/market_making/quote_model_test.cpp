#include "engine/strategy/market_making/fair/FairValueModel.h"
#include "engine/strategy/market_making/quote/InventorySkewModel.h"
#include "engine/strategy/market_making/quote/QuoteEngine.h"
#include "engine/strategy/market_making/quote/QuoteSizeModel.h"
#include "engine/strategy/market_making/quote/SpreadModel.h"
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

void expect_false(bool value, const std::string& field) {
    if (value) {
        fail("expected false: " + field);
    }
}

MarketMakingConfig config() {
    MarketMakingConfig cfg;
    cfg.min_half_spread_tick = 1'000;
    cfg.fee_buffer_tick = 2'000;
    cfg.latency_buffer_tick = 3'000;
    cfg.max_inventory_skew_tick = 10'000;
    cfg.base_quote_size_lots = 5;
    cfg.max_inventory_lots = 100;
    return cfg;
}

void SpreadModel_UsesMaxBuffers() {
    const auto spread = SpreadModel{}.compute(config());
    expect_equal(spread.half_spread_tick, 3'000LL, "spread");
}

void InventorySkew_LongInventoryShiftsQuotesDown() {
    const auto cfg = config();
    const auto skew = InventorySkewModel{}.compute(cfg, 50);
    expect_equal(skew, 5'000LL, "skew");
    const auto fair = 500'000LL;
    const auto half = 10'000LL;
    expect_equal(fair - half - skew, 485'000LL, "bid");
    expect_equal(fair + half - skew, 505'000LL, "ask");
}

void QuoteSizeModel_ReducesBidNearInventoryLimit() {
    auto depth = tools::make_depth_view(490'000, 510'000, 20.0, 20.0, 1, 100);
    auto cfg = config();
    cfg.base_quote_size_lots = 10;
    const auto result = QuoteSizeModel{}.compute(cfg, depth, 95);
    expect_true(result.ok, "ok");
    expect_equal(result.bid_qty_lots, 5LL, "bid qty");
}

void QuoteEngine_BuildsBidAsk() {
    auto depth = tools::make_depth_view(490'000, 510'000, 20.0, 20.0, 1, 100);
    auto cfg = config();
    cfg.min_half_spread_tick = 5'000;
    cfg.fee_buffer_tick = 0;
    cfg.latency_buffer_tick = 0;
    const auto fair = FairValueModel{}.compute(depth, cfg, 100);
    const auto spread = SpreadModel{}.compute(cfg);
    const auto size = QuoteSizeModel{}.compute(cfg, depth, 20);
    const auto build = QuoteEngine{}.build(QuoteBuildInput{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .market_index = 1,
        .asset_index = 7,
        .depth = &depth,
        .config = &cfg,
        .fair_value = fair,
        .spread = spread,
        .size = size,
        .inventory_skew_tick = 0,
        .current_position_lots = 20,
        .now_ns = 100
    });
    expect_true(build.ok, "ok");
    expect_true(build.quote.has_bid, "bid");
    expect_true(build.quote.has_ask, "ask");
    expect_equal(build.quote.bid.price_tick, 495'000LL, "bid price");
    expect_equal(build.quote.ask.price_tick, 505'000LL, "ask price");
}

void QuoteEngine_RejectsCrossedQuote() {
    auto depth = tools::make_depth_view(490'000, 510'000, 20.0, 20.0, 1, 100);
    auto cfg = config();
    cfg.min_half_spread_tick = 0;
    cfg.fee_buffer_tick = 0;
    cfg.latency_buffer_tick = 0;
    const auto build = QuoteEngine{}.build(QuoteBuildInput{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .asset_index = 7,
        .depth = &depth,
        .config = &cfg,
        .fair_value = FairValueResult{.ok = true, .fair_value_tick = 1},
        .spread = SpreadResult{.half_spread_tick = 0},
        .size = QuoteSizeResult{.ok = true, .bid_qty_lots = 1, .ask_qty_lots = 1},
        .now_ns = 100
    });
    expect_false(build.ok, "ok");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> map{
        {"SpreadModel_UsesMaxBuffers", &SpreadModel_UsesMaxBuffers},
        {"InventorySkew_LongInventoryShiftsQuotesDown", &InventorySkew_LongInventoryShiftsQuotesDown},
        {"QuoteSizeModel_ReducesBidNearInventoryLimit", &QuoteSizeModel_ReducesBidNearInventoryLimit},
        {"QuoteEngine_BuildsBidAsk", &QuoteEngine_BuildsBidAsk},
        {"QuoteEngine_RejectsCrossedQuote", &QuoteEngine_RejectsCrossedQuote}
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
