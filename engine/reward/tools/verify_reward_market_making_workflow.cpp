#include "engine/execution/adapter/PaperMakerExecutionAdapter.h"
#include "engine/paper/ledger/PaperEventAdapter.h"
#include "engine/paper/ledger/PaperLedger.h"
#include "engine/paper/pnl/MakerPnLEngine.h"
#include "engine/paper/pnl/RewardPnLEngine.h"
#include "engine/reward/normalize/RewardMarketNormalizer.h"
#include "engine/reward/store/RewardUniverseStore.h"
#include "engine/risk/quote/QuoteRiskEvaluator.h"
#include "engine/strategy/market_making/core/MarketMakingEngine.h"
#include "engine/strategy/market_making/tools/MarketMakingTools.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

namespace execution = trading_engine::execution;
namespace paper = trading_engine::paper;
namespace reward = trading_engine::reward;
namespace risk = trading_engine::risk;
namespace mm = trading_engine::strategy::market_making;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

reward::RewardConfigSnapshot reward_fixture() {
    reward::RawRewardMarketConfig raw;
    raw.condition_id = "condition-1";
    raw.market_slug = "solana-above-60";
    raw.reward_asset_symbol = "USDC";
    raw.rewards_max_spread = 0.02;
    raw.rewards_min_size = 12.0;
    raw.total_daily_rate = 1.0;
    raw.active = true;
    raw.tokens.push_back({.token_id = "asset_yes", .outcome = "YES"});
    return reward::RewardMarketNormalizer{}.normalize_snapshot(
        {raw},
        1'000,
        "verify_reward_fixture"
    );
}

risk::QuoteRiskPolicy quote_policy() {
    risk::QuoteRiskPolicy policy;
    policy.max_quote_qty_lots = 100;
    policy.max_quote_notional_tick = 100'000'000;
    policy.max_asset_inventory_lots = 100;
    policy.max_book_age_ns = 10'000'000'000ULL;
    policy.min_replace_interval_ns = 0;
    policy.reward_eligibility_required = true;
    policy.reward_max_spread_tick = 20'000;
    policy.reward_min_quote_size_lots = 12;
    return policy;
}

}  // namespace

int main() {
    try {
        const auto rewards = reward_fixture();
        reward::RewardUniverseStore store;
        store.update(rewards);
        if (!store.by_token_id("asset_yes")) {
            fail("reward store lookup failed");
        }

        auto depth = mm::tools::make_depth_view(
            490'000,
            510'000,
            30.0,
            30.0,
            7,
            1'000
        );
        depth.last_ws_recv_ns = 1'000;

        mm::MarketMakingConfig config;
        config.min_half_spread_tick = 20'000;
        config.base_quote_size_lots = 5;
        config.target_position_lots = 20;
        config.max_inventory_lots = 100;
        config.quote_ttl_ns = 5'000'000'000ULL;
        config.reward_aware_quotes_enabled = true;
        config.reward_min_size_lots_floor = 12;
        config.reward_max_spread_tick_buffer = 0;

        mm::MarketMakingEngine strategy{config};
        const auto strategy_result = strategy.on_market_update(mm::MarketMakingInput{
            .market_id = "condition-1",
            .asset_id = "asset_yes",
            .asset_index = 7,
            .depth = &depth,
            .current_position_lots = 20,
            .reward_config = &store.latest_snapshot(),
            .now_ns = 2'000,
        });
        if (strategy_result.quote_count != 1) {
            fail("strategy did not emit quote");
        }
        const auto& quote = strategy_result.quotes[0];
        if (!quote.reward_eligible) {
            fail("quote was not reward eligible");
        }

        const auto policy = quote_policy();
        const auto risk_result = risk::QuoteRiskEvaluator{}.evaluate(
            risk::QuoteRiskInput{
                .quote = &quote,
                .depth = &depth,
                .policy = &policy,
                .current_position_lots = 20,
                .now_ns = 3'000,
            }
        );
        if (!risk_result.approved_quote.has_value()) {
            fail("risk rejected reward quote");
        }

        execution::PaperMakerExecutionAdapter execution_adapter{
            execution::PaperMakerFillMode::Conservative
        };
        const auto submit = execution_adapter.submit_approved_quote(
            *risk_result.approved_quote,
            4'000
        );
        if (!submit.ok) {
            fail("paper maker submit failed");
        }

        const execution::PaperMakerMarketEvent event{
            .ts_ns = 5'000,
            .asset_index = 7,
            .depth = &depth,
            .has_trade = true,
            .trade_price_tick = quote.bid.price_tick,
            .trade_qty_lots = 3,
            .trade_aggressor_side = execution::OrderSide::Sell,
        };
        const auto reports = execution_adapter.on_market_event(event);

        paper::PaperLedger ledger{100'000'000'000LL};
        paper::PaperEventAdapter paper_adapter;
        for (const auto& report : reports) {
            const auto adapted = paper_adapter.observe(report);
            if (adapted.has_paper_fill) {
                const auto applied = ledger.apply_fill(adapted.paper_fill);
                if (!applied.applied) {
                    fail("paper ledger did not apply maker fill");
                }
            }
        }

        const std::vector<trading_engine::state::MarketDepthView> depths{depth};
        const auto maker_pnl = paper::MakerPnLEngine{}.compute(
            ledger,
            depths,
            6'000
        );

        const paper::RewardQuoteObservation reward_observation{
            .condition_id = quote.reward_condition_id,
            .asset_id = quote.asset_id,
            .asset_index = quote.asset_index,
            .has_bid = quote.has_bid,
            .has_ask = quote.has_ask,
            .bid_price_tick = quote.bid.price_tick,
            .ask_price_tick = quote.ask.price_tick,
            .bid_size_lots = quote.bid.quantity_lots,
            .ask_size_lots = quote.ask.quantity_lots,
            .start_ts_ns = 4'000,
            .end_ts_ns = 3'604'000'000'000ULL,
        };
        const auto reward_pnl = paper::RewardPnLEngine{}.compute(
            store.latest_snapshot(),
            {&reward_observation, 1},
            nullptr,
            3'604'000'000'000ULL
        );

        std::cout << "reward_markets: " << store.latest_snapshot().markets.size()
                  << "\n";
        std::cout << "quote_reward_eligible: "
                  << (quote.reward_eligible ? "true" : "false") << "\n";
        std::cout << "risk_decision: "
                  << risk::quote_risk_decision_type_name(
                         risk_result.decision.decision
                     )
                  << "\n";
        std::cout << "maker_reports: " << reports.size() << "\n";
        std::cout << "maker_fills: " << ledger.snapshot().maker_fill_count
                  << "\n";
        std::cout << "maker_realized_pnl: "
                  << maker_pnl.maker_realized_pnl_tick << "\n";
        std::cout << "reward_eligible_seconds: "
                  << reward_pnl.eligible_quote_seconds << "\n";
        std::cout << "reward_accrued_tick_estimate: "
                  << reward_pnl.reward_accrued_tick_estimate << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
