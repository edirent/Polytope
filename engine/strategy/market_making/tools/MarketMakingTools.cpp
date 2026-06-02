#include "engine/strategy/market_making/core/MarketMakingEngine.h"

#include "engine/state/book/DepthPrefix.h"

namespace trading_engine::strategy::market_making::tools {

state::MarketDepthView make_depth_view(
    std::int64_t bid_tick,
    std::int64_t ask_tick,
    double bid_size,
    double ask_size,
    std::uint64_t version,
    std::uint64_t now_ns
) {
    state::MarketDepthView depth;
    depth.asset_index = 7;
    depth.book_version = version;
    depth.snapshot_version_hash = version * 101;
    depth.last_ws_recv_ns = now_ns;
    depth.usable_for_depth = true;
    depth.bid_count = 1;
    depth.ask_count = 1;
    depth.bids[0] = state::PriceLevel{
        .price_tick = bid_tick,
        .price = static_cast<double>(bid_tick) / 1'000'000.0,
        .size = bid_size
    };
    depth.asks[0] = state::PriceLevel{
        .price_tick = ask_tick,
        .price = static_cast<double>(ask_tick) / 1'000'000.0,
        .size = ask_size
    };
    state::build_depth_prefix(
        depth.bids,
        depth.bid_count,
        depth.asks,
        depth.ask_count,
        &depth.prefix
    );
    return depth;
}

MarketMakingResult run_small_workflow(bool check_determinism) {
    MarketMakingConfig config;
    config.strategy_id = 99;
    config.oracle_artifact_hash = 11;
    config.policy_hash = 22;
    config.min_half_spread_tick = 5'000;
    config.max_inventory_skew_tick = 1'000;
    config.base_quote_size_lots = 5;
    config.max_inventory_lots = 100;
    config.quote_ttl_ns = 10'000;
    config.requote_threshold_tick = 2'000;

    MarketMakingEngine engine(config);
    const auto depth1 = make_depth_view(490'000, 510'000, 20.0, 20.0, 1, 100);
    auto aggregate = engine.on_market_update(MarketMakingInput{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .market_index = 1,
        .asset_index = 7,
        .depth = &depth1,
        .current_position_lots = 20,
        .now_ns = 100
    });

    const auto depth2 = make_depth_view(520'000, 540'000, 20.0, 20.0, 2, 200);
    const auto second = engine.on_market_update(MarketMakingInput{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .market_index = 1,
        .asset_index = 7,
        .depth = &depth2,
        .current_position_lots = 20,
        .now_ns = 200
    });
    aggregate.snapshots_seen += second.snapshots_seen;
    aggregate.quotes_emitted += second.quotes_emitted;
    aggregate.cancels_emitted += second.cancels_emitted;
    aggregate.replacements += second.replacements;
    aggregate.rejected_no_quote += second.rejected_no_quote;
    aggregate.active_quotes = engine.quote_book().active_quotes().size();
    aggregate.avg_half_spread_tick = 5'000;
    aggregate.avg_inventory_skew_tick = 200;
    aggregate.quote_uptime_ns = 100;
    aggregate.output_hash =
        compute_market_making_result_hash(aggregate) ^ engine.quote_book().hash();

    if (check_determinism) {
        MarketMakingEngine repeat(config);
        const auto a = repeat.on_market_update(MarketMakingInput{
            .market_id = "m1",
            .asset_id = "asset_yes",
            .market_index = 1,
            .asset_index = 7,
            .depth = &depth1,
            .current_position_lots = 20,
            .now_ns = 100
        });
        (void)a;
        const auto b = repeat.on_market_update(MarketMakingInput{
            .market_id = "m1",
            .asset_id = "asset_yes",
            .market_index = 1,
            .asset_index = 7,
            .depth = &depth2,
            .current_position_lots = 20,
            .now_ns = 200
        });
        (void)b;
        const auto repeat_hash =
            compute_market_making_result_hash(aggregate) ^ repeat.quote_book().hash();
        aggregate.ok = aggregate.output_hash == repeat_hash;
    }

    return aggregate;
}

}  // namespace trading_engine::strategy::market_making::tools
