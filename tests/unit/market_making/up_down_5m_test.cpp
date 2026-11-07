#include "engine/strategy/market_making/fair/FairValueModel.h"
#include "engine/strategy/market_making/products/up_down_5m/UpDown5mQuoteController.h"
#include "engine/strategy/market_making/quote/QuoteEngine.h"
#include "engine/strategy/market_making/tools/MarketMakingTools.h"

#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

namespace mm = trading_engine::strategy::market_making;
namespace product =
    trading_engine::strategy::market_making::products::up_down_5m;
namespace reward = trading_engine::reward;

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

product::UpDown5mMarketContext context_for_z(
    double z,
    std::int64_t tau_ms,
    mm::ProductDirection asset_direction = mm::ProductDirection::Up
) {
    constexpr double start = 70'000.0;
    constexpr double sigma = 0.80;
    constexpr std::int64_t now_ms = 1'000'000;
    const auto tau_years =
        (static_cast<double>(tau_ms) / 1000.0) /
        (365.0 * 24.0 * 60.0 * 60.0);
    const auto current =
        start * std::exp(z * sigma * std::sqrt(tau_years));
    return product::UpDown5mMarketContext{
        .market_id = "btc-up-down-5m",
        .up_token_id = "up",
        .down_token_id = "down",
        .asset_direction = asset_direction,
        .round_start_ms = now_ms - (300'000 - tau_ms),
        .round_expiry_ms = now_ms + tau_ms,
        .now_ms = now_ms,
        .start_spot = start,
        .current_spot = current,
        .sigma_annualized = sigma,
        .oracle_age_ms = 50.0,
        .polymarket_up_mid = 0.50,
        .polymarket_down_mid = 0.50,
        .settlement_source_ok = true,
        .book_ok = true
    };
}

reward::RewardConfigSnapshot reward_snapshot(
    std::int64_t max_spread_tick,
    std::int64_t min_size_lots
) {
    reward::RewardConfigSnapshot snapshot;
    reward::RewardMarketConfig market;
    market.condition_id = "m1";
    market.rewards_max_spread_tick = max_spread_tick;
    market.rewards_min_size_lots = min_size_lots;
    market.tokens.push_back(
        reward::RewardTokenConfig{
            .token_id = "asset_up",
            .outcome = "UP",
            .eligible = true
        }
    );
    snapshot.markets.push_back(market);
    return snapshot;
}

mm::QuoteBuildInput base_quote_input(
    const trading_engine::state::MarketDepthView* depth,
    const mm::MarketMakingConfig* config,
    const reward::RewardConfigSnapshot* rewards
) {
    return mm::QuoteBuildInput{
        .market_id = "m1",
        .asset_id = "asset_up",
        .asset_index = 7,
        .depth = depth,
        .config = config,
        .fair_value = mm::FairValueResult{
            .ok = true,
            .fair_value_tick = 500'000,
            .external_fair_value_tick = 500'000,
            .quality = mm::FairValueQuality::Valid,
            .source = mm::FairValueSourceKind::External
        },
        .spread = mm::SpreadResult{.half_spread_tick = 10'000},
        .size = mm::QuoteSizeResult{
            .ok = true,
            .bid_qty_lots = 5,
            .ask_qty_lots = 5
        },
        .reward_config = rewards,
        .inventory_skew_tick = 0,
        .current_position_lots = 0,
        .product_kind = mm::ProductKind::TerminalDigital5m,
        .product_asset_direction = mm::ProductDirection::Up,
        .product_winning_direction = mm::ProductDirection::Unknown,
        .product_external_fair_tick = 500'000,
        .product_z_score_bps = 8'000,
        .product_tau_ms = 120'000,
        .product_dynamic_inventory_cap_lots = 100,
        .product_opening_allowed = true,
        .product_settlement_source_ok = true,
        .now_ns = 1'000
    };
}

void UpDown5mFairProvider_AtTheMoneyNearHalf() {
    auto ctx = context_for_z(0.0, 240'000);
    ctx.current_spot = ctx.start_spot;
    const auto fair = product::UpDown5mFairProvider{}.compute(ctx);
    expect_true(fair.ok, "fair ok");
    expect_true(fair.up_fair_tick > 450'000, "up fair lower");
    expect_true(fair.up_fair_tick < 550'000, "up fair upper");
    expect_equal(fair.down_fair_tick, mm::kPriceOneTick - fair.up_fair_tick, "down");
}

void UpDown5mQuoteController_GatesByTauAndZ() {
    const product::UpDown5mQuoteController controller;
    const auto early = controller.build_plan(context_for_z(0.8, 240'000), 0);
    expect_true(early.opening_allowed, "early z 0.8");
    expect_true(early.dynamic_inventory_cap_lots > 0, "early cap");

    const auto mid = controller.build_plan(context_for_z(0.8, 120'000), 0);
    expect_false(mid.opening_allowed, "mid z 0.8");
    expect_equal(mid.dynamic_inventory_cap_lots, 0LL, "mid cap");

    const auto late = controller.build_plan(context_for_z(0.5, 45'000), 0);
    expect_false(late.opening_allowed, "late z 0.5");

    const auto hard = controller.build_plan(context_for_z(0.1, 20'000), 0);
    expect_true(hard.cancel_all_quotes, "hard cancel");
}

void UpDown5mQuoteController_DropsLosingSideWhenZHigh() {
    const product::UpDown5mQuoteController controller;
    const auto plan = controller.build_plan(
        context_for_z(1.6, 240'000, mm::ProductDirection::Down),
        0
    );
    expect_false(plan.opening_allowed, "opening");
    expect_equal(plan.winning_direction, mm::ProductDirection::Up, "winner");
    expect_true(plan.disable_bid_quotes, "bid disabled without inventory");
    expect_true(plan.disable_ask_quotes, "ask disabled without inventory");
}

void UpDown5mQuoteController_InventoryCapShrinksNearExpiry() {
    const product::UpDown5mQuoteController controller;
    const auto early = controller.build_plan(context_for_z(0.2, 240'000), 0);
    const auto near = controller.build_plan(context_for_z(0.2, 90'000), 0);
    expect_true(early.dynamic_inventory_cap_lots > near.dynamic_inventory_cap_lots, "shrinks");
    expect_true(near.dynamic_inventory_cap_lots > 0, "near nonzero");
}

void RewardConstraintDoesNotLiftOpeningSizeFor5mDigital() {
    auto depth = mm::tools::make_depth_view(490'000, 510'000, 30.0, 30.0, 1, 1'000);
    auto config = mm::MarketMakingConfig{};
    config.reward_aware_quotes_enabled = true;
    const auto rewards = reward_snapshot(50'000, 20);
    auto input = base_quote_input(&depth, &config, &rewards);
    input.product_reward_size_lift_allowed = false;
    input.product_reward_spread_compression_allowed = true;

    const auto build = mm::QuoteEngine{}.build(input);
    expect_true(build.ok, build.reason);
    expect_equal(build.quote.bid.quantity_lots, 5LL, "bid qty");
    expect_equal(build.quote.ask.quantity_lots, 5LL, "ask qty");
    expect_false(build.quote.reward_eligible, "reward eligible");
}

void RewardConstraintDoesNotCompressSpreadWhenZTooHighFor5mDigital() {
    auto depth = mm::tools::make_depth_view(490'000, 510'000, 30.0, 30.0, 1, 1'000);
    auto config = mm::MarketMakingConfig{};
    config.reward_aware_quotes_enabled = true;
    const auto rewards = reward_snapshot(2'000, 1);
    auto input = base_quote_input(&depth, &config, &rewards);
    input.product_reward_size_lift_allowed = true;
    input.product_reward_spread_compression_allowed = false;

    const auto build = mm::QuoteEngine{}.build(input);
    expect_true(build.ok, build.reason);
    expect_true(
        build.quote.ask.price_tick - build.quote.bid.price_tick > 2'000,
        "spread unchanged"
    );
    expect_false(build.quote.reward_eligible, "reward eligible");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> map{
        {"UpDown5mFairProvider_AtTheMoneyNearHalf", &UpDown5mFairProvider_AtTheMoneyNearHalf},
        {"UpDown5mQuoteController_GatesByTauAndZ", &UpDown5mQuoteController_GatesByTauAndZ},
        {"UpDown5mQuoteController_DropsLosingSideWhenZHigh", &UpDown5mQuoteController_DropsLosingSideWhenZHigh},
        {"UpDown5mQuoteController_InventoryCapShrinksNearExpiry", &UpDown5mQuoteController_InventoryCapShrinksNearExpiry},
        {"RewardConstraintDoesNotLiftOpeningSizeFor5mDigital", &RewardConstraintDoesNotLiftOpeningSizeFor5mDigital},
        {"RewardConstraintDoesNotCompressSpreadWhenZTooHighFor5mDigital", &RewardConstraintDoesNotCompressSpreadWhenZTooHighFor5mDigital}
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
