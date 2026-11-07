#include "engine/strategy/market_making/products/up_down_5m/UpDown5mQuoteController.h"

#include <algorithm>
#include <cmath>

namespace trading_engine::strategy::market_making::products::up_down_5m {

namespace {

constexpr double kSecondsPerYear = 365.0 * 24.0 * 60.0 * 60.0;

[[nodiscard]] ProductDirection winning_direction(
    const UpDown5mMarketContext& context
) noexcept {
    if (context.current_spot > context.start_spot) {
        return ProductDirection::Up;
    }
    if (context.current_spot < context.start_spot) {
        return ProductDirection::Down;
    }
    return ProductDirection::Unknown;
}

[[nodiscard]] bool allow_opening_quotes(
    const UpDown5mQuoteControllerConfig& config,
    std::uint64_t tau_ms,
    std::int64_t z_score_bps
) noexcept {
    const auto tau_sec = static_cast<double>(tau_ms) / 1000.0;
    if (tau_sec > 180.0) {
        return z_score_bps < config.z_gate_tau_gt_180_bps;
    }
    if (tau_sec > 60.0) {
        return z_score_bps < config.z_gate_tau_60_180_bps;
    }
    if (tau_sec > 30.0) {
        return z_score_bps < config.z_gate_tau_30_60_bps;
    }
    return false;
}

[[nodiscard]] double inventory_cap_multiplier(
    std::uint64_t tau_ms,
    std::int64_t z_score_bps
) noexcept {
    const auto tau_sec = static_cast<double>(tau_ms) / 1000.0;
    double t_mult = 0.0;
    if (tau_sec > 180.0) {
        t_mult = 1.0;
    } else if (tau_sec > 120.0) {
        t_mult = 0.6;
    } else if (tau_sec > 60.0) {
        t_mult = 0.3;
    }

    double z_mult = 0.0;
    if (z_score_bps < 5'000) {
        z_mult = 1.0;
    } else if (z_score_bps < 10'000) {
        z_mult = 0.5;
    } else if (z_score_bps < 15'000) {
        z_mult = 0.2;
    }
    return t_mult * z_mult;
}

[[nodiscard]] std::uint64_t quote_ttl_ms(
    const UpDown5mQuoteControllerConfig& config,
    std::uint64_t tau_ms
) noexcept {
    const auto tau_sec = static_cast<double>(tau_ms) / 1000.0;
    if (tau_sec > 180.0) {
        return config.ttl_ms_tau_gt_180;
    }
    if (tau_sec > 60.0) {
        return config.ttl_ms_tau_60_180;
    }
    if (tau_sec > 30.0) {
        return config.ttl_ms_tau_30_60;
    }
    return 0;
}

[[nodiscard]] std::int64_t fair_move_tick(
    const UpDown5mQuoteControllerConfig& config,
    std::uint64_t tau_ms
) noexcept {
    return tau_ms > 180'000 ? config.fair_move_tick_tau_gt_180
                            : config.fair_move_tick_tau_lt_180;
}

[[nodiscard]] std::int64_t clamp_half_spread(
    const UpDown5mQuoteControllerConfig& config,
    std::int64_t value
) noexcept {
    return std::clamp(
        value,
        config.min_half_spread_tick,
        config.max_half_spread_tick
    );
}

[[nodiscard]] double normal_pdf(double value) noexcept {
    constexpr double kInvSqrtTwoPi = 0.39894228040143267794;
    return kInvSqrtTwoPi * std::exp(-0.5 * value * value);
}

[[nodiscard]] double digital_delta_prob_per_dollar(
    const UpDown5mMarketContext& context,
    double tau_years
) noexcept {
    if (tau_years <= 0.0 || context.sigma_annualized <= 0.0 ||
        context.current_spot <= 0.0 || context.start_spot <= 0.0) {
        return 0.0;
    }
    const auto denom = context.sigma_annualized * std::sqrt(tau_years);
    if (denom <= 0.0 || !std::isfinite(denom)) {
        return 0.0;
    }
    const auto d =
        (std::log(context.current_spot / context.start_spot) -
         0.5 * context.sigma_annualized * context.sigma_annualized * tau_years) /
        denom;
    if (!std::isfinite(d)) {
        return 0.0;
    }
    return normal_pdf(d) / (context.current_spot * denom);
}

[[nodiscard]] std::int64_t terminal_gamma_buffer_tick(
    const UpDown5mQuoteControllerConfig& config,
    const UpDown5mMarketContext& context,
    std::uint64_t tau_ms
) noexcept {
    if (tau_ms == 0 || context.current_spot <= 0.0 ||
        context.sigma_annualized <= 0.0) {
        return 0;
    }
    const auto tau_years =
        (static_cast<double>(tau_ms) / 1000.0) / kSecondsPerYear;
    const auto delta =
        digital_delta_prob_per_dollar(context, tau_years);
    if (delta <= 0.0 || !std::isfinite(delta)) {
        return 0;
    }

    const auto latency_ms = std::max<double>(
        static_cast<double>(config.gamma_latency_floor_ms),
        std::max(0.0, context.oracle_age_ms)
    );
    const auto spot_vol_per_sqrt_second =
        context.current_spot * context.sigma_annualized /
        std::sqrt(kSecondsPerYear);
    const auto expected_stale_move_usd =
        spot_vol_per_sqrt_second * std::sqrt(latency_ms / 1000.0);
    const auto gamma_probability =
        (static_cast<double>(config.gamma_buffer_multiplier_bps) / 10'000.0) *
        delta * expected_stale_move_usd;
    if (!std::isfinite(gamma_probability) || gamma_probability <= 0.0) {
        return 0;
    }
    return static_cast<std::int64_t>(
        std::llround(gamma_probability * static_cast<double>(kPriceOneTick))
    );
}

[[nodiscard]] std::int64_t expiry_buffer_tick(std::uint64_t tau_ms) noexcept {
    const auto tau_sec = static_cast<double>(tau_ms) / 1000.0;
    std::int64_t buffer = 0;
    if (tau_sec < 180.0) {
        buffer += 10'000;
    }
    if (tau_sec < 120.0) {
        buffer += 20'000;
    }
    if (tau_sec < 60.0) {
        buffer += 40'000;
    }
    return buffer;
}

[[nodiscard]] std::int64_t oracle_staleness_buffer_tick(
    const UpDown5mQuoteControllerConfig& config,
    const UpDown5mMarketContext& context
) noexcept {
    if (config.max_oracle_age_ms == 0 || context.oracle_age_ms <= 0.0) {
        return 0;
    }
    const auto ratio = context.oracle_age_ms /
        static_cast<double>(config.max_oracle_age_ms);
    if (ratio <= 0.5) {
        return 0;
    }
    return static_cast<std::int64_t>(
        std::llround(std::min(1.0, ratio) * 10'000.0)
    );
}

[[nodiscard]] std::int64_t asset_mid_tick(
    const UpDown5mMarketContext& context
) noexcept {
    const auto mid = context.asset_direction == ProductDirection::Up
        ? context.polymarket_up_mid
        : context.polymarket_down_mid;
    if (!std::isfinite(mid) || mid <= 0.0) {
        return 0;
    }
    return std::clamp<std::int64_t>(
        static_cast<std::int64_t>(
            std::llround(mid * static_cast<double>(kPriceOneTick))
        ),
        1,
        kPriceOneTick - 1
    );
}

[[nodiscard]] double target_time_decay(std::uint64_t tau_ms) noexcept {
    const auto tau_sec = static_cast<double>(tau_ms) / 1000.0;
    if (tau_sec > 180.0) {
        return 1.0;
    }
    if (tau_sec > 60.0) {
        return 0.5;
    }
    return 0.0;
}

[[nodiscard]] std::int64_t directional_target_lots(
    const UpDown5mQuoteControllerConfig& config,
    const UpDown5mMarketContext& context,
    const UpDown5mFairOutput& fair,
    std::uint64_t tau_ms,
    std::int64_t cap_lots
) noexcept {
    if (!config.allow_directional_inventory || cap_lots <= 0 ||
        config.directional_edge_scale_tick <= 0) {
        return 0;
    }
    const auto mid_tick = asset_mid_tick(context);
    if (mid_tick <= 0) {
        return 0;
    }
    const auto edge_tick = fair.asset_fair_tick - mid_tick;
    const auto raw =
        static_cast<double>(cap_lots) *
        std::tanh(
            static_cast<double>(edge_tick) /
            static_cast<double>(config.directional_edge_scale_tick)
        ) *
        target_time_decay(tau_ms);
    return std::clamp<std::int64_t>(
        static_cast<std::int64_t>(std::llround(raw)),
        -config.directional_soft_cap_lots,
        config.directional_soft_cap_lots
    );
}

}  // namespace

void UpDown5mQuotePlan::apply_to(MarketMakingInput* input) const noexcept {
    if (!input) {
        return;
    }
    input->external_fair_value_tick = external_fair_value_tick;
    input->canonical_yes_fair_value_tick = canonical_yes_fair_value_tick;
    input->canonical_yes_position_lots = current_canonical_yes_position_lots;
    input->target_canonical_yes_lots = target_canonical_yes_lots;
    input->dynamic_target_position_lots = dynamic_target_position_lots;
    input->dynamic_min_inventory_lots = dynamic_min_inventory_lots;
    input->dynamic_max_inventory_lots = dynamic_max_inventory_lots;
    input->dynamic_half_spread_tick = dynamic_half_spread_tick;
    input->dynamic_max_inventory_skew_tick = dynamic_max_inventory_skew_tick;
    input->dynamic_quote_ttl_ns = dynamic_quote_ttl_ns;
    input->dynamic_requote_threshold_tick = dynamic_requote_threshold_tick;
    input->dynamic_max_book_age_ns = dynamic_max_book_age_ns;
    input->disable_bid_quotes = disable_bid_quotes;
    input->disable_ask_quotes = disable_ask_quotes;
    input->product_kind = ProductKind::TerminalDigital5m;
    input->product_asset_direction = asset_direction;
    input->product_winning_direction = winning_direction;
    input->product_external_fair_tick = external_fair_value_tick;
    input->product_z_score_bps = z_score_bps;
    input->product_tau_ms = tau_ms;
    input->product_dynamic_inventory_cap_lots = dynamic_inventory_cap_lots;
    input->product_opening_allowed = opening_allowed;
    input->product_settlement_source_ok = settlement_source_ok;
    input->product_reward_size_lift_allowed = reward_size_lift_allowed;
    input->product_reward_spread_compression_allowed =
        reward_spread_compression_allowed;
    input->time_to_expiry_ns = tau_ms * 1'000'000ULL;
}

UpDown5mQuoteController::UpDown5mQuoteController(
    UpDown5mQuoteControllerConfig config
)
    : config_(config) {}

UpDown5mQuotePlan UpDown5mQuoteController::build_plan(
    const UpDown5mMarketContext& context,
    std::int64_t current_asset_position_lots
) const {
    UpDown5mQuotePlan plan;
    plan.asset_direction = context.asset_direction;
    plan.winning_direction = winning_direction(context);
    plan.settlement_source_ok = context.settlement_source_ok;
    plan.current_canonical_yes_position_lots =
        context.asset_direction == ProductDirection::Down
            ? -current_asset_position_lots
            : current_asset_position_lots;
    plan.dynamic_max_inventory_skew_tick = config_.max_inventory_skew_tick;
    plan.dynamic_max_book_age_ns = config_.max_book_age_ns;

    if (!config_.enabled) {
        plan.cancel_all_quotes = true;
        plan.disable_bid_quotes = true;
        plan.disable_ask_quotes = true;
        plan.reason = "up_down_5m_disabled";
        return plan;
    }

    const auto fair = fair_provider_.compute(context);
    if (!fair.ok) {
        plan.cancel_all_quotes = true;
        plan.disable_bid_quotes = true;
        plan.disable_ask_quotes = true;
        plan.reason = fair.reject_reason;
        return plan;
    }

    plan.ok = true;
    plan.external_fair_value_tick = fair.asset_fair_tick;
    plan.canonical_yes_fair_value_tick = fair.up_fair_tick;
    plan.z_score_bps = fair.z_score_bps;
    plan.tau_ms = fair.tau_ms;

    const auto oracle_stale =
        context.oracle_age_ms >
        static_cast<double>(config_.max_oracle_age_ms);
    const auto hard_cancel =
        fair.tau_ms < config_.hard_cancel_tau_ms ||
        !context.settlement_source_ok || !context.book_ok || oracle_stale;
    const auto losing_side_hard_cut =
        plan.winning_direction != ProductDirection::Unknown &&
        plan.asset_direction != ProductDirection::Unknown &&
        plan.asset_direction != plan.winning_direction &&
        fair.z_score_bps > config_.losing_side_hard_cutoff_bps;
    plan.opening_allowed =
        !hard_cancel && !losing_side_hard_cut &&
        allow_opening_quotes(config_, fair.tau_ms, fair.z_score_bps);

    const auto cap_multiplier = plan.opening_allowed
        ? inventory_cap_multiplier(fair.tau_ms, fair.z_score_bps)
        : 0.0;
    const auto cap_lots = std::max<std::int64_t>(
        0,
        static_cast<std::int64_t>(
            std::llround(
                static_cast<double>(config_.base_max_canonical_lots) *
                cap_multiplier
            )
        )
    );

    plan.dynamic_inventory_cap_lots = cap_lots;
    plan.dynamic_min_inventory_lots = 0;
    plan.dynamic_max_inventory_lots = cap_lots;
    plan.dynamic_target_position_lots =
        directional_target_lots(config_, context, fair, fair.tau_ms, cap_lots);
    plan.target_canonical_yes_lots =
        context.asset_direction == ProductDirection::Down
            ? -plan.dynamic_target_position_lots
            : plan.dynamic_target_position_lots;

    const auto expiry_buffer = expiry_buffer_tick(fair.tau_ms);
    const auto gamma_buffer =
        terminal_gamma_buffer_tick(config_, context, fair.tau_ms);
    const auto staleness_buffer =
        oracle_staleness_buffer_tick(config_, context);
    plan.dynamic_half_spread_tick = clamp_half_spread(
        config_,
        config_.base_half_spread_tick + expiry_buffer + gamma_buffer +
            staleness_buffer
    );

    const auto ttl_ms = quote_ttl_ms(config_, fair.tau_ms);
    plan.dynamic_quote_ttl_ns = ttl_ms * 1'000'000ULL;
    plan.dynamic_requote_threshold_tick = fair_move_tick(config_, fair.tau_ms);
    plan.cancel_all_quotes = hard_cancel || ttl_ms == 0;
    plan.disable_bid_quotes = plan.cancel_all_quotes;
    plan.disable_ask_quotes = plan.cancel_all_quotes;
    if (!plan.opening_allowed && current_asset_position_lots == 0) {
        plan.disable_bid_quotes = true;
        plan.disable_ask_quotes = true;
    }

    plan.reward_size_lift_allowed =
        plan.opening_allowed && fair.tau_ms > 180'000 &&
        fair.z_score_bps <= 7'000 && !oracle_stale;
    plan.reward_spread_compression_allowed =
        plan.reward_size_lift_allowed;
    plan.reason = plan.opening_allowed ? "opening_allowed"
                                       : "reduce_only_or_cancel";
    return plan;
}

}  // namespace trading_engine::strategy::market_making::products::up_down_5m
