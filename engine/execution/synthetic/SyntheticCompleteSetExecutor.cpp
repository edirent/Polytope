#include "engine/execution/synthetic/SyntheticCompleteSetExecutor.h"

namespace trading_engine::execution::synthetic {

SyntheticCompleteSetDecision SyntheticCompleteSetExecutor::evaluate_paper(
    const SyntheticCompleteSetInput& input
) const noexcept {
    SyntheticCompleteSetDecision out;
    if (input.rich_leg_side != strategy::market_making::OutcomeSide::No) {
        out.reason = "only_no_rich_leg_enabled";
        return out;
    }
    if (input.rich_leg_bid_tick <= 0 ||
        input.rich_leg_tradable_fair_tick <= 0 ||
        input.cheap_leg_tradable_fair_tick <= 0) {
        out.reason = "missing_fair_or_bid";
        return out;
    }
    if (input.residual_inventory_lots >=
        input.target_residual_inventory_lots) {
        out.reason = "residual_inventory_at_target";
        return out;
    }
    if (input.exit_depth_lots < input.min_exit_depth_lots) {
        out.reason = "exit_liquidity_too_low";
        return out;
    }
    if (input.available_cash_tick < 1'000'000) {
        out.reason = "cash_insufficient";
        return out;
    }
    if (input.tte_ns > 0 && input.tte_ns < 30'000'000'000LL) {
        out.reason = "expiry_puke_zone";
        return out;
    }

    out.synthetic_cheap_leg_cost_tick = 1'000'000 - input.rich_leg_bid_tick;
    out.expected_edge_tick =
        input.cheap_leg_tradable_fair_tick -
        out.synthetic_cheap_leg_cost_tick;
    if (out.expected_edge_tick < input.min_edge_tick) {
        out.reason = "edge_too_low";
        return out;
    }
    out.should_mint_and_sell_rich_leg = true;
    out.reason = "mint_sell_rich_no_keep_yes";
    return out;
}

}  // namespace trading_engine::execution::synthetic
