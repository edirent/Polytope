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

constexpr std::uint64_t kNsPerSecond = 1'000'000'000ULL;

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

void InventorySkew_UsesTargetInventoryAsNeutral() {
    auto cfg = config();
    cfg.min_inventory_lots = 0;
    cfg.target_position_lots = 50;
    cfg.max_inventory_lots = 100;
    expect_equal(InventorySkewModel{}.compute(cfg, 50), 0LL, "neutral");
    expect_equal(InventorySkewModel{}.compute(cfg, 100), 10'000LL, "max long");
    expect_equal(InventorySkewModel{}.compute(cfg, 0), -10'000LL, "max short");
}

void InventorySkew_TteAmplifiesNearExpiry() {
    auto cfg = config();
    cfg.tte_skew_start_ns = 120 * kNsPerSecond;
    cfg.tte_puke_start_ns = 30 * kNsPerSecond;
    cfg.tte_max_skew_multiplier = 4.0;

    const auto base = InventorySkewModel{}.compute(cfg, 50);
    const auto far = InventorySkewModel{}.compute(cfg, 50, 180 * kNsPerSecond);
    const auto near = InventorySkewModel{}.compute(cfg, 50, 60 * kNsPerSecond);
    const auto puke = InventorySkewModel{}.compute(cfg, 50, 20 * kNsPerSecond);

    expect_equal(base, 5'000LL, "base skew");
    expect_equal(far, base, "far tte skew");
    expect_true(near > base, "near tte amplifies");
    expect_true(near < puke, "puke tte largest");
    expect_equal(puke, 20'000LL, "puke skew");
}

void InventorySkew_NonlinearPressureAcceleratesPastThreshold() {
    auto cfg = config();
    cfg.inventory_skew_nonlinear_start_bps = 2'000;
    cfg.inventory_skew_exponent = 2.0;

    const auto linear = InventorySkewModel{}.compute(config(), 60);
    const auto nonlinear = InventorySkewModel{}.compute(cfg, 60);

    expect_true(nonlinear > linear, "nonlinear pressure");
}

void QuoteSizeModel_ReducesBidNearInventoryLimit() {
    auto depth = tools::make_depth_view(490'000, 510'000, 20.0, 20.0, 1, 100);
    auto cfg = config();
    cfg.base_quote_size_lots = 10;
    const auto result = QuoteSizeModel{}.compute(cfg, depth, 95);
    expect_true(result.ok, "ok");
    expect_equal(result.bid_qty_lots, 1LL, "bid qty");
}

void QuoteEngine_BuildsBidAsk() {
    auto depth = tools::make_depth_view(490'000, 510'000, 20.0, 20.0, 1, 100);
    auto cfg = config();
    cfg.min_half_spread_tick = 5'000;
    cfg.fee_buffer_tick = 0;
    cfg.latency_buffer_tick = 0;
    cfg.target_position_lots = 20;
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

void QuoteEngine_DisablesBidSide() {
    auto depth = tools::make_depth_view(490'000, 510'000, 20.0, 20.0, 1, 100);
    auto cfg = config();
    cfg.min_half_spread_tick = 5'000;
    cfg.fee_buffer_tick = 0;
    cfg.latency_buffer_tick = 0;
    cfg.enable_bid_quotes = false;
    const auto fair = FairValueModel{}.compute(depth, cfg, 100);
    const auto build = QuoteEngine{}.build(QuoteBuildInput{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .market_index = 1,
        .asset_index = 7,
        .depth = &depth,
        .config = &cfg,
        .fair_value = fair,
        .spread = SpreadModel{}.compute(cfg),
        .size = QuoteSizeModel{}.compute(cfg, depth, 20),
        .inventory_skew_tick = 0,
        .current_position_lots = 20,
        .now_ns = 100
    });
    expect_true(build.ok, "ok");
    expect_false(build.quote.has_bid, "bid");
    expect_true(build.quote.has_ask, "ask");
}

void QuoteEngine_AllowsReducingAskBelowFair() {
    auto depth = tools::make_depth_view(490'000, 510'000, 20.0, 20.0, 1, 100);
    auto cfg = config();
    cfg.min_half_spread_tick = 1;
    cfg.fee_buffer_tick = 0;
    cfg.latency_buffer_tick = 0;
    const auto fair = FairValueModel{}.compute(depth, cfg, 100);
    const auto size = QuoteSizeResult{.ok = true, .bid_qty_lots = 1, .ask_qty_lots = 1};
    const auto build = QuoteEngine{}.build(QuoteBuildInput{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .market_index = 1,
        .asset_index = 7,
        .depth = &depth,
        .config = &cfg,
        .fair_value = fair,
        .spread = SpreadResult{.half_spread_tick = 1},
        .size = size,
        .inventory_skew_tick = 75'000,
        .current_position_lots = 99,
        .now_ns = 100
    });
    expect_true(build.ok, "ok");
    expect_false(build.quote.has_bid, "bid disabled in unwind");
    expect_true(build.quote.has_ask, "ask");
    expect_true(build.quote.ask.price_tick < build.quote.fair_value_tick, "reducing ask below fair");
    expect_equal(build.quote.type, QuoteIntentType::ForcedUnwind, "unwind type");
    expect_equal(build.quote.risk_mode, QuoteIntentRiskMode::ForcedReduce, "risk mode");
    expect_true(build.quote.ask.risk_reducing, "ask risk reducing");
    expect_true(build.quote.ask.allow_fair_deviation_exemption, "ask fair exemption");
    expect_true(build.quote.ask.allow_spread_exemption, "ask spread exemption");
}

void QuoteEngine_PassiveReduceAskJoinsBestAsk() {
    auto depth = tools::make_depth_view(490'000, 510'000, 20.0, 20.0, 1, 100);
    auto cfg = config();
    cfg.min_half_spread_tick = 10'000;
    cfg.fee_buffer_tick = 0;
    cfg.latency_buffer_tick = 0;
    cfg.target_position_lots = 0;
    cfg.max_inventory_lots = 100;
    cfg.passive_unwind_position_bps = 0;
    cfg.forced_unwind_position_bps = 9'000;
    cfg.passive_reduce_excess_lots = 20;
    cfg.urgent_reduce_excess_lots = 50;
    cfg.passive_reduce_join_tick = 1;
    const auto fair = FairValueModel{}.compute(depth, cfg, 100);
    const auto build = QuoteEngine{}.build(QuoteBuildInput{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .market_index = 1,
        .asset_index = 7,
        .depth = &depth,
        .config = &cfg,
        .fair_value = fair,
        .spread = SpreadResult{.half_spread_tick = 10'000},
        .size = QuoteSizeResult{.ok = true, .bid_qty_lots = 10, .ask_qty_lots = 10},
        .inventory_skew_tick = 0,
        .current_position_lots = 25,
        .now_ns = 100
    });
    expect_true(build.ok, "ok");
    expect_false(build.quote.has_bid, "no bid in passive reduce");
    expect_true(build.quote.has_ask, "ask");
    expect_equal(build.quote.ask.price_tick, 509'999LL, "joins best ask");
    expect_equal(build.quote.risk_mode, QuoteIntentRiskMode::ReduceOnly, "risk mode");
    expect_true(build.quote.ask.risk_reducing, "ask risk reducing");
}

void QuoteEngine_UrgentReduceAskMovesToMidpoint() {
    auto depth = tools::make_depth_view(490'000, 510'000, 20.0, 20.0, 1, 100);
    auto cfg = config();
    cfg.min_half_spread_tick = 10'000;
    cfg.fee_buffer_tick = 0;
    cfg.latency_buffer_tick = 0;
    cfg.target_position_lots = 0;
    cfg.max_inventory_lots = 100;
    cfg.passive_unwind_position_bps = 0;
    cfg.forced_unwind_position_bps = 9'000;
    cfg.passive_reduce_excess_lots = 20;
    cfg.urgent_reduce_excess_lots = 50;
    const auto fair = FairValueModel{}.compute(depth, cfg, 100);
    const auto build = QuoteEngine{}.build(QuoteBuildInput{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .market_index = 1,
        .asset_index = 7,
        .depth = &depth,
        .config = &cfg,
        .fair_value = fair,
        .spread = SpreadResult{.half_spread_tick = 10'000},
        .size = QuoteSizeResult{.ok = true, .bid_qty_lots = 10, .ask_qty_lots = 10},
        .inventory_skew_tick = 0,
        .current_position_lots = 60,
        .now_ns = 100
    });
    expect_true(build.ok, "ok");
    expect_true(build.quote.has_ask, "ask");
    expect_equal(build.quote.ask.price_tick, 500'000LL, "urgent ask at mid");
    expect_equal(build.quote.type, QuoteIntentType::PassiveUnwind, "urgent type");
    expect_equal(build.quote.risk_mode, QuoteIntentRiskMode::ReduceOnly, "risk mode");
}

void QuoteEngine_ReduceOnlyQtyCappedToTarget() {
    auto depth = tools::make_depth_view(490'000, 510'000, 20.0, 20.0, 1, 100);
    auto cfg = config();
    cfg.min_half_spread_tick = 10'000;
    cfg.fee_buffer_tick = 0;
    cfg.latency_buffer_tick = 0;
    cfg.target_position_lots = 50;
    cfg.max_inventory_lots = 100;
    cfg.passive_unwind_position_bps = 0;
    cfg.forced_unwind_position_bps = 9'000;
    cfg.passive_reduce_excess_lots = 1;
    cfg.urgent_reduce_excess_lots = 50;
    const auto fair = FairValueModel{}.compute(depth, cfg, 100);
    const auto build = QuoteEngine{}.build(QuoteBuildInput{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .market_index = 1,
        .asset_index = 7,
        .depth = &depth,
        .config = &cfg,
        .fair_value = fair,
        .spread = SpreadResult{.half_spread_tick = 10'000},
        .size = QuoteSizeResult{.ok = true, .bid_qty_lots = 10, .ask_qty_lots = 10},
        .inventory_skew_tick = 0,
        .current_position_lots = 55,
        .now_ns = 100
    });
    expect_true(build.ok, "ok");
    expect_true(build.quote.has_ask, "ask");
    expect_equal(build.quote.ask.quantity_lots, 5LL, "qty capped to target");
}

void QuoteEngine_ClampsOpeningAskToCorrectSideOfFair() {
    auto depth = tools::make_depth_view(490'000, 510'000, 20.0, 20.0, 1, 100);
    auto cfg = config();
    cfg.min_half_spread_tick = 1;
    cfg.fee_buffer_tick = 0;
    cfg.latency_buffer_tick = 0;
    const auto fair = FairValueModel{}.compute(depth, cfg, 100);
    const auto size = QuoteSizeResult{.ok = true, .bid_qty_lots = 1, .ask_qty_lots = 1};
    const auto build = QuoteEngine{}.build(QuoteBuildInput{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .market_index = 1,
        .asset_index = 7,
        .depth = &depth,
        .config = &cfg,
        .fair_value = fair,
        .spread = SpreadResult{.half_spread_tick = 1},
        .size = size,
        .inventory_skew_tick = 75'000,
        .current_position_lots = 0,
        .now_ns = 100
    });
    expect_true(build.ok, "ok");
    expect_true(build.quote.has_ask, "ask");
    expect_true(build.quote.ask.price_tick > build.quote.fair_value_tick, "opening ask above fair");
    expect_equal(build.quote.ask.price_tick, build.quote.fair_value_tick + 1, "ask clamped");
}

void QuoteEngine_RequiresExternalFairForOpeningQuotes() {
    auto depth = tools::make_depth_view(490'000, 510'000, 20.0, 20.0, 1, 100);
    auto cfg = config();
    cfg.min_half_spread_tick = 5'000;
    cfg.fee_buffer_tick = 0;
    cfg.latency_buffer_tick = 0;
    cfg.require_external_fair_for_opening_quotes = true;
    cfg.target_position_lots = 20;
    const auto fair = FairValueModel{}.compute(depth, cfg, 100);
    const auto build = QuoteEngine{}.build(QuoteBuildInput{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .market_index = 1,
        .asset_index = 7,
        .depth = &depth,
        .config = &cfg,
        .fair_value = fair,
        .spread = SpreadModel{}.compute(cfg),
        .size = QuoteSizeModel{}.compute(cfg, depth, 20),
        .inventory_skew_tick = 0,
        .current_position_lots = 20,
        .now_ns = 100
    });
    expect_false(build.ok, "opening quotes blocked without external fair");
}

void QuoteEngine_DropsOpeningBidWhenReducingAskWouldSelfCross() {
    auto depth = tools::make_depth_view(150'000, 160'000, 20.0, 20.0, 1, 100);
    auto cfg = config();
    cfg.fee_buffer_tick = 0;
    cfg.latency_buffer_tick = 0;
    cfg.min_inventory_lots = 0;
    cfg.target_position_lots = 0;
    cfg.max_inventory_lots = 100;
    cfg.passive_reduce_join_tick = 1;
    const auto build = QuoteEngine{}.build(QuoteBuildInput{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .market_index = 1,
        .asset_index = 7,
        .depth = &depth,
        .config = &cfg,
        .fair_value = FairValueResult{
            .ok = true,
            .fair_value_tick = 178'595,
            .external_fair_value_tick = 178'595,
            .quality = FairValueQuality::Valid,
            .source = FairValueSourceKind::External
        },
        .spread = SpreadResult{.half_spread_tick = 13'406},
        .size = QuoteSizeResult{
            .ok = true,
            .bid_qty_lots = 9,
            .ask_qty_lots = 9
        },
        .inventory_skew_tick = 0,
        .current_position_lots = 9,
        .now_ns = 100
    });
    expect_true(build.ok, "ok");
    expect_false(build.quote.has_bid, "opening bid dropped");
    expect_true(build.quote.has_ask, "reducing ask kept");
    expect_equal(build.quote.ask.price_tick, 159'999LL, "ask joins best ask");
    expect_true(build.quote.ask.risk_reducing, "ask risk reducing");
    expect_equal(
        build.quote.risk_mode,
        QuoteIntentRiskMode::ReduceOnly,
        "risk mode"
    );
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
        {"InventorySkew_UsesTargetInventoryAsNeutral", &InventorySkew_UsesTargetInventoryAsNeutral},
        {"InventorySkew_TteAmplifiesNearExpiry", &InventorySkew_TteAmplifiesNearExpiry},
        {"InventorySkew_NonlinearPressureAcceleratesPastThreshold", &InventorySkew_NonlinearPressureAcceleratesPastThreshold},
        {"QuoteSizeModel_ReducesBidNearInventoryLimit", &QuoteSizeModel_ReducesBidNearInventoryLimit},
        {"QuoteEngine_BuildsBidAsk", &QuoteEngine_BuildsBidAsk},
        {"QuoteEngine_DisablesBidSide", &QuoteEngine_DisablesBidSide},
        {"QuoteEngine_AllowsReducingAskBelowFair", &QuoteEngine_AllowsReducingAskBelowFair},
        {"QuoteEngine_PassiveReduceAskJoinsBestAsk", &QuoteEngine_PassiveReduceAskJoinsBestAsk},
        {"QuoteEngine_UrgentReduceAskMovesToMidpoint", &QuoteEngine_UrgentReduceAskMovesToMidpoint},
        {"QuoteEngine_ReduceOnlyQtyCappedToTarget", &QuoteEngine_ReduceOnlyQtyCappedToTarget},
        {"QuoteEngine_ClampsOpeningAskToCorrectSideOfFair", &QuoteEngine_ClampsOpeningAskToCorrectSideOfFair},
        {"QuoteEngine_RequiresExternalFairForOpeningQuotes", &QuoteEngine_RequiresExternalFairForOpeningQuotes},
        {"QuoteEngine_DropsOpeningBidWhenReducingAskWouldSelfCross", &QuoteEngine_DropsOpeningBidWhenReducingAskWouldSelfCross},
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
