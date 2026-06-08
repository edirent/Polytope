#include "engine/paper/pnl/RewardPnLEngine.h"
#include "engine/paper/read/DashboardReadStore.h"
#include "engine/reward/features/RewardMarketFeatureBuilder.h"
#include "engine/reward/filter/RewardMarketFilter.h"
#include "engine/reward/normalize/RewardMarketNormalizer.h"
#include "engine/reward/store/RewardUniverseStore.h"
#include "engine/risk/quote/QuoteRiskEvaluator.h"
#include "engine/strategy/market_making/fair/FairValueModel.h"
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

namespace mm = trading_engine::strategy::market_making;
namespace paper = trading_engine::paper;
namespace reward = trading_engine::reward;
namespace risk = trading_engine::risk;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& label) {
    if (!value) {
        fail("expected true: " + label);
    }
}

template <typename Actual, typename Expected>
void expect_equal(
    const Actual& actual,
    const Expected& expected,
    const std::string& label
) {
    if (!(actual == expected)) {
        fail("mismatch: " + label);
    }
}

reward::RawRewardMarketConfig raw_fixture() {
    reward::RawRewardMarketConfig raw;
    raw.condition_id = "condition-1";
    raw.market_slug = "solana-above-60";
    raw.reward_asset_symbol = "USDC";
    raw.rewards_max_spread = 0.02;
    raw.rewards_min_size = 12.2;
    raw.rate_per_day = 1.5;
    raw.total_daily_rate = 1.5;
    raw.active = true;
    raw.tokens.push_back({.token_id = "asset_yes", .outcome = "YES"});
    raw.tokens.push_back({.token_id = "asset_no", .outcome = "NO"});
    return raw;
}

reward::RewardConfigSnapshot reward_snapshot() {
    return reward::RewardMarketNormalizer{}.normalize_snapshot(
        {raw_fixture()},
        1'000,
        "fixture"
    );
}

risk::QuoteRiskPolicy reward_policy() {
    risk::QuoteRiskPolicy policy;
    policy.max_quote_qty_lots = 100;
    policy.max_quote_notional_tick = 100'000'000;
    policy.max_asset_inventory_lots = 100;
    policy.max_book_age_ns = 1'000'000'000;
    policy.min_replace_interval_ns = 0;
    policy.reward_eligibility_required = true;
    policy.reward_max_spread_tick = 20'000;
    policy.reward_min_quote_size_lots = 12;
    return policy;
}

mm::QuoteIntent quote_fixture() {
    mm::QuoteIntent quote;
    quote.quote_intent_id = 11;
    quote.quote_group_id = 12;
    quote.type = mm::QuoteIntentType::PlaceQuote;
    quote.market_id = "condition-1";
    quote.asset_id = "asset_yes";
    quote.asset_index = 7;
    quote.has_bid = true;
    quote.has_ask = true;
    quote.fair_value_tick = 500'000;
    quote.created_ts_ns = 100;
    quote.expires_at_ns = 2'000;
    quote.snapshot_version_hash = 99;
    quote.idempotency_hash = 101;
    quote.bid.market_id = quote.market_id;
    quote.bid.asset_id = quote.asset_id;
    quote.bid.asset_index = quote.asset_index;
    quote.bid.side = mm::QuoteSide::Bid;
    quote.bid.price_tick = 490'000;
    quote.bid.quantity_lots = 12;
    quote.bid.fair_value_tick = quote.fair_value_tick;
    quote.bid.edge_to_fair_tick = 10'000;
    quote.ask = quote.bid;
    quote.ask.side = mm::QuoteSide::Ask;
    quote.ask.price_tick = 510'000;
    quote.ask.edge_to_fair_tick = 10'000;
    return quote;
}

trading_engine::state::MarketDepthView depth_fixture() {
    return mm::tools::make_depth_view(
        490'000,
        510'000,
        30.0,
        30.0,
        7,
        100
    );
}

void RewardNormalizer_NormalizesFixture() {
    const auto snapshot = reward_snapshot();
    expect_equal(snapshot.source_quality, reward::RewardSourceQuality::Good, "quality");
    expect_true(snapshot.source_version_hash != 0, "hash");
    expect_equal(snapshot.markets.size(), static_cast<std::size_t>(1), "market count");
    const auto& market = snapshot.markets[0];
    expect_equal(market.reward_asset, reward::RewardAsset::Usdc, "asset");
    expect_equal(market.rewards_max_spread_tick, 20'000LL, "spread tick");
    expect_equal(market.rewards_min_size_lots, 13LL, "min size ceil");
    expect_equal(market.tokens.size(), static_cast<std::size_t>(2), "tokens");
}

void RewardUniverseStore_LookupByConditionAndToken() {
    reward::RewardUniverseStore store;
    store.update(reward_snapshot());
    expect_true(store.refresh_ts_ns() == 1'000, "refresh");
    const auto* by_condition = store.by_condition_id("condition-1");
    const auto* by_token = store.by_token_id("asset_no");
    expect_true(by_condition != nullptr, "condition lookup");
    expect_true(by_token != nullptr, "token lookup");
    expect_equal(by_condition->condition_id, by_token->condition_id, "same market");
}

void RewardMarketFilter_KeepsEligibleFeature() {
    const auto snapshot = reward_snapshot();
    const auto feature = reward::RewardMarketFeatureBuilder{}.build(snapshot.markets[0]);
    expect_true(reward::RewardMarketFilter{}.keep(feature), "filter keep");
}

void QuoteEngine_RewardConstraintAdjustsQuote() {
    auto depth = depth_fixture();
    auto cfg = mm::MarketMakingConfig{};
    cfg.min_half_spread_tick = 5'000;
    cfg.base_quote_size_lots = 5;
    cfg.target_position_lots = 20;
    cfg.max_inventory_lots = 100;
    cfg.reward_aware_quotes_enabled = true;
    cfg.reward_min_size_lots_floor = 12;
    cfg.reward_max_spread_tick_buffer = 0;
    const auto fair = mm::FairValueModel{}.compute(depth, cfg, 1'000);
    const auto size = mm::QuoteSizeModel{}.compute(cfg, depth, 20);
    const auto rewards = reward_snapshot();
    const auto build = mm::QuoteEngine{}.build(mm::QuoteBuildInput{
        .market_id = "condition-1",
        .asset_id = "asset_yes",
        .asset_index = 7,
        .depth = &depth,
        .config = &cfg,
        .fair_value = fair,
        .spread = mm::SpreadModel{}.compute(cfg),
        .size = size,
        .reward_config = &rewards,
        .inventory_skew_tick = 0,
        .current_position_lots = 20,
        .now_ns = 1'000
    });
    expect_true(build.ok, build.reason);
    expect_true(build.quote.reward_eligible, "reward eligible");
    expect_true(build.quote.has_bid && build.quote.has_ask, "two sided");
    expect_true(
        build.quote.ask.price_tick - build.quote.bid.price_tick <= 20'000,
        "spread cap"
    );
    expect_true(build.quote.bid.quantity_lots >= 13, "bid min size");
    expect_true(build.quote.ask.quantity_lots >= 13, "ask min size");
}

void QuoteRisk_RewardMissingConfigRejected() {
    const auto quote = quote_fixture();
    const auto depth = depth_fixture();
    const auto policy = reward_policy();
    const auto result = risk::QuoteRiskEvaluator{}.evaluate(risk::QuoteRiskInput{
        .quote = &quote,
        .depth = &depth,
        .policy = &policy,
        .now_ns = 1'000
    });
    expect_equal(
        result.decision.decision,
        risk::QuoteRiskDecisionType::RejectRewardConfigMissing,
        "decision"
    );
}

void QuoteRisk_RewardSizeTooSmallRejected() {
    auto quote = quote_fixture();
    quote.reward_config_present = true;
    quote.reward_max_spread_tick = 20'000;
    quote.reward_min_size_lots = 20;
    quote.reward_eligible = false;
    const auto depth = depth_fixture();
    auto policy = reward_policy();
    policy.reward_min_quote_size_lots = 20;
    const auto result = risk::QuoteRiskEvaluator{}.evaluate(risk::QuoteRiskInput{
        .quote = &quote,
        .depth = &depth,
        .policy = &policy,
        .now_ns = 1'000
    });
    expect_equal(
        result.decision.decision,
        risk::QuoteRiskDecisionType::RejectRewardSizeTooSmall,
        "decision"
    );
}

void RewardPnLEngine_ComputesSeparateAccrualAndDashboard() {
    auto rewards = reward_snapshot();
    rewards.markets[0].rewards_min_size_lots = 10;
    rewards.markets[0].total_daily_rate = 1.0;
    const paper::RewardQuoteObservation observation{
        .condition_id = "condition-1",
        .asset_id = "asset_yes",
        .asset_index = 7,
        .has_bid = true,
        .has_ask = true,
        .bid_price_tick = 490'000,
        .ask_price_tick = 510'000,
        .bid_size_lots = 10,
        .ask_size_lots = 10,
        .start_ts_ns = 0,
        .end_ts_ns = 86'400'000'000'000ULL,
    };
    const auto snapshot =
        paper::RewardPnLEngine{}.compute(rewards, {&observation, 1}, nullptr, 2'000);
    expect_equal(snapshot.eligible_quote_seconds, 86'400ULL, "seconds");
    expect_equal(snapshot.reward_accrued_tick_estimate, 1'000'000LL, "accrual");
    const auto dashboard = paper::reward_dashboard_from_pnl(snapshot);
    expect_equal(
        dashboard.reward_accrued_tick_estimate,
        snapshot.reward_accrued_tick_estimate,
        "dashboard accrual"
    );
    expect_equal(dashboard.reward_source_quality, std::string{"good"}, "quality");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            fail("expected one test case");
        }
        const std::string test_case{argv[1]};
        const std::unordered_map<std::string, void (*)()> tests{
            {"RewardNormalizer_NormalizesFixture", RewardNormalizer_NormalizesFixture},
            {"RewardUniverseStore_LookupByConditionAndToken", RewardUniverseStore_LookupByConditionAndToken},
            {"RewardMarketFilter_KeepsEligibleFeature", RewardMarketFilter_KeepsEligibleFeature},
            {"QuoteEngine_RewardConstraintAdjustsQuote", QuoteEngine_RewardConstraintAdjustsQuote},
            {"QuoteRisk_RewardMissingConfigRejected", QuoteRisk_RewardMissingConfigRejected},
            {"QuoteRisk_RewardSizeTooSmallRejected", QuoteRisk_RewardSizeTooSmallRejected},
            {"RewardPnLEngine_ComputesSeparateAccrualAndDashboard", RewardPnLEngine_ComputesSeparateAccrualAndDashboard},
        };
        const auto it = tests.find(test_case);
        if (it == tests.end()) {
            fail("unknown test case: " + test_case);
        }
        it->second();
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}
