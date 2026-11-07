#include "engine/risk/quote/QuoteRiskEvaluator.h"
#include "engine/strategy/market_making/core/MarketMakingEngine.h"
#include "engine/strategy/market_making/products/up_down_5m/UpDown5mFairProvider.h"
#include "engine/strategy/market_making/products/up_down_5m/UpDown5mQuoteController.h"
#include "engine/strategy/market_making/tools/MarketMakingTools.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace mm = trading_engine::strategy::market_making;
namespace updown =
    trading_engine::strategy::market_making::products::up_down_5m;
namespace risk = trading_engine::risk;
namespace state = trading_engine::state;

constexpr std::uint64_t kNsPerSecond = 1'000'000'000ULL;
constexpr std::int64_t kRoundSeconds = 300;
constexpr std::int64_t kTotalSeconds = 30 * 60;
constexpr std::uint32_t kUpAssetIndex = 701;
constexpr std::uint32_t kDownAssetIndex = 702;

struct TokenRuntime {
    mm::ProductDirection direction = mm::ProductDirection::Unknown;
    std::string asset_id;
    std::uint32_t asset_index = 0;
    mm::MarketMakingEngine engine;
    struct ActiveReplayQuote {
        risk::ApprovedQuote approved;
        mm::QuoteIntentType type = mm::QuoteIntentType::None;
        mm::ProductDirection token_direction = mm::ProductDirection::Unknown;
        std::int64_t tau_ms = 0;
        std::int64_t z_score_bps = 0;
        std::int64_t fair_at_quote_tick = 0;
        std::int64_t best_bid_tick = 0;
        std::int64_t best_ask_tick = 0;
    };
    std::optional<ActiveReplayQuote> active_quote;
    std::int64_t position_lots = 0;
    std::int64_t cash_tick = 0;
};

struct QuoteValidityMetrics {
    std::uint64_t post_only_cross_rejects = 0;
    std::uint64_t approved_crossing_quotes = 0;
    std::uint64_t negative_opening_edge_quotes = 0;
    std::int64_t max_bid_cross_tick = 0;
    std::int64_t max_ask_cross_tick = 0;
};

struct RegimeStats {
    std::uint64_t fills = 0;
    std::int64_t spread_capture_tick = 0;
};

struct Metrics {
    std::uint64_t seconds = 0;
    std::uint64_t snapshots = 0;
    std::uint64_t plans_ok = 0;
    std::uint64_t opening_allowed = 0;
    std::uint64_t controller_cancels = 0;
    std::uint64_t no_quote = 0;
    std::uint64_t quote_intents = 0;
    std::uint64_t cancel_intents = 0;
    std::uint64_t risk_approved = 0;
    std::uint64_t risk_rejected = 0;
    std::uint64_t fills = 0;
    std::uint64_t bid_fills = 0;
    std::uint64_t ask_fills = 0;
    std::int64_t traded_lots = 0;
    std::int64_t traded_notional_tick = 0;
    std::int64_t spread_capture_tick = 0;
    std::int64_t settlement_pnl_tick = 0;
    std::int64_t max_abs_canonical_lots = 0;
    std::array<std::uint64_t, risk::kQuoteRiskDecisionTypeCount> risk_counts{};
    QuoteValidityMetrics quote_validity;
    std::array<std::int64_t, 2> spread_capture_by_token{};
    std::array<std::int64_t, 2> spread_capture_by_side{};
    std::array<std::int64_t, 3> spread_capture_by_purpose{};
    std::array<RegimeStats, 7> fills_by_regime{};
};

struct RealInputRow {
    std::int64_t second = 0;
    std::int64_t round = 0;
    std::string market_slug;
    std::string condition_id;
    std::string up_token_id;
    std::string down_token_id;
    std::int64_t round_start_ms = 0;
    std::int64_t round_expiry_ms = 0;
    double start_spot = 0.0;
    double current_spot = 0.0;
    double sigma_annualized = 0.55;
    std::int64_t up_bid_tick = 0;
    std::int64_t up_ask_tick = 0;
    std::int64_t down_bid_tick = 0;
    std::int64_t down_ask_tick = 0;
    std::int64_t up_buy_trade_tick = 0;
    std::int64_t up_sell_trade_tick = 0;
    std::int64_t down_buy_trade_tick = 0;
    std::int64_t down_sell_trade_tick = 0;
};

struct BookTicks {
    std::int64_t bid_tick = 0;
    std::int64_t ask_tick = 0;
};

[[nodiscard]] std::int64_t clamp_tick(std::int64_t value) noexcept {
    return std::clamp<std::int64_t>(value, 1, mm::kPriceOneTick - 1);
}

[[nodiscard]] std::int64_t to_tick(double probability) noexcept {
    return clamp_tick(static_cast<std::int64_t>(
        std::llround(probability * static_cast<double>(mm::kPriceOneTick))
    ));
}

[[nodiscard]] double spot_path(std::int64_t second) noexcept {
    constexpr double kPi = 3.14159265358979323846;
    const auto t = static_cast<double>(second);
    const auto cycle = 72.0 * std::sin(2.0 * kPi * t / 515.0);
    const auto fast = 26.0 * std::sin(2.0 * kPi * t / 83.0);
    const auto micro = 9.0 * std::sin(2.0 * kPi * t / 17.0);
    const auto drift = 0.015 * t;
    const auto shock = second >= 780 && second < 930 ? 48.0 : 0.0;
    const auto fade = second >= 1260 ? -0.09 * static_cast<double>(second - 1260) : 0.0;
    return 65'000.0 + cycle + fast + micro + drift + shock + fade;
}

[[nodiscard]] std::int64_t synthetic_lag_tick(std::int64_t second) noexcept {
    constexpr double kPi = 3.14159265358979323846;
    const auto t = static_cast<double>(second);
    return static_cast<std::int64_t>(
        std::llround(
            18'000.0 * std::sin(2.0 * kPi * t / 37.0) +
            7'000.0 * std::sin(2.0 * kPi * t / 11.0)
        )
    );
}

[[nodiscard]] state::MarketDepthView make_depth(
    std::int64_t mid_tick,
    std::uint32_t asset_index,
    std::uint64_t version,
    std::uint64_t now_ns
) {
    const auto half_spread = static_cast<std::int64_t>(
        8'000 + (version % 13 == 0 ? 2'000 : 0)
    );
    auto depth = mm::tools::make_depth_view(
        clamp_tick(mid_tick - half_spread),
        clamp_tick(mid_tick + half_spread),
        80.0,
        80.0,
        version,
        now_ns
    );
    depth.asset_index = asset_index;
    depth.snapshot_version_hash = version * 101 + asset_index;
    return depth;
}

[[nodiscard]] state::MarketDepthView make_depth_from_bid_ask(
    std::int64_t bid_tick,
    std::int64_t ask_tick,
    std::uint32_t asset_index,
    std::uint64_t version,
    std::uint64_t now_ns
) {
    auto depth = mm::tools::make_depth_view(
        clamp_tick(bid_tick),
        clamp_tick(std::max(ask_tick, bid_tick + 1)),
        80.0,
        80.0,
        version,
        now_ns
    );
    depth.asset_index = asset_index;
    depth.snapshot_version_hash = version * 101 + asset_index;
    return depth;
}

[[nodiscard]] BookTicks reconstructed_book_ticks(
    std::int64_t bid_tick,
    std::int64_t ask_tick,
    std::int64_t mid_tick,
    std::int64_t z_score_bps,
    std::int64_t second,
    std::int64_t conservative_base_half_spread_tick
) noexcept {
    if (conservative_base_half_spread_tick <= 0) {
        return BookTicks{
            .bid_tick = bid_tick,
            .ask_tick = ask_tick
        };
    }
    const auto z_add =
        std::min<std::int64_t>(20'000, std::max<std::int64_t>(0, z_score_bps) / 2);
    const auto time_add = (second % 5) * 500;
    const auto half_spread =
        conservative_base_half_spread_tick + z_add + time_add;
    return BookTicks{
        .bid_tick = clamp_tick(mid_tick - half_spread),
        .ask_tick = clamp_tick(mid_tick + half_spread)
    };
}

[[nodiscard]] mm::MarketMakingConfig market_making_config(
    std::uint64_t strategy_id
) noexcept {
    mm::MarketMakingConfig config;
    config.strategy_id = strategy_id;
    config.oracle_artifact_hash = 5'005;
    config.policy_hash = 30'005;
    config.min_half_spread_tick = 15'000;
    config.max_inventory_skew_tick = 75'000;
    config.base_quote_size_lots = 5;
    config.max_quote_size_multiplier_bps = 10'000;
    config.min_quote_edge_tick = 0;
    config.min_top_depth_lots = 1;
    config.min_total_depth_lots = 1;
    config.min_fair_confidence_bps = 1;
    config.external_fair_weight_bps = 10'000;
    config.require_external_fair_for_opening_quotes = true;
    config.max_inventory_lots = 80;
    config.passive_unwind_position_bps = 3'000;
    config.forced_unwind_position_bps = 9'000;
    config.passive_reduce_excess_lots = 8;
    config.urgent_reduce_excess_lots = 20;
    config.passive_reduce_join_tick = 1;
    config.quote_ttl_ns = 1'500'000'000ULL;
    config.requote_threshold_tick = 1;
    config.min_requote_interval_ns = 0;
    config.max_book_age_ns = 500'000'000LL;
    return config;
}

[[nodiscard]] risk::QuoteRiskPolicy risk_policy() noexcept {
    risk::QuoteRiskPolicy policy;
    policy.max_quote_qty_lots = 20;
    policy.max_quote_notional_tick = 20'000'000;
    policy.max_asset_inventory_lots = 80;
    policy.min_edge_to_fair_tick = 0;
    policy.max_quote_fair_deviation_tick = 250'000;
    policy.max_canonical_yes_exposure_lots = 80;
    policy.max_spot_age_ms = 300;
    policy.max_vol_age_ms = 60'000;
    policy.min_external_confidence_bps = 8'000;
    policy.max_quote_age_ns = 2'000'000'000ULL;
    policy.max_book_age_ns = 500'000'000ULL;
    policy.min_replace_interval_ns = 0;
    policy.max_active_quotes_per_asset = 2;
    policy.terminal_digital_5m_guard_enabled = true;
    policy.terminal_digital_min_quote_tau_ms = 30'000;
    policy.terminal_digital_max_oracle_age_ms = 300;
    policy.terminal_digital_max_opening_z_bps = 10'000;
    policy.terminal_digital_losing_side_z_cutoff_bps = 15'000;
    policy.terminal_digital_max_fair_deviation_tick = 80'000;
    return policy;
}

[[nodiscard]] std::int64_t canonical_position(
    const TokenRuntime& up,
    const TokenRuntime& down
) noexcept {
    return up.position_lots - down.position_lots;
}

[[nodiscard]] std::int64_t canonical_delta(
    mm::ProductDirection direction,
    mm::QuoteSide side,
    std::int64_t quantity_lots
) noexcept {
    const auto asset_delta = side == mm::QuoteSide::Bid
        ? quantity_lots
        : -quantity_lots;
    return direction == mm::ProductDirection::Down
        ? -asset_delta
        : asset_delta;
}

[[nodiscard]] const char* token_side_name(
    mm::ProductDirection direction
) noexcept {
    switch (direction) {
        case mm::ProductDirection::Up:
            return "up";
        case mm::ProductDirection::Down:
            return "down";
        case mm::ProductDirection::Unknown:
            return "unknown";
    }
    return "unknown";
}

[[nodiscard]] const char* quote_side_name(mm::QuoteSide side) noexcept {
    return side == mm::QuoteSide::Bid ? "bid" : "ask";
}

[[nodiscard]] const char* canonical_side_name(std::int64_t delta) noexcept {
    if (delta > 0) {
        return "long_yes";
    }
    if (delta < 0) {
        return "short_yes";
    }
    return "flat";
}

[[nodiscard]] const char* quote_purpose_name(
    mm::QuoteIntentType type
) noexcept {
    if (type == mm::QuoteIntentType::ForcedUnwind) {
        return "forced_unwind";
    }
    if (type == mm::QuoteIntentType::PassiveUnwind) {
        return "passive_unwind";
    }
    return "opening";
}

[[nodiscard]] std::size_t token_index(mm::ProductDirection direction) noexcept {
    return direction == mm::ProductDirection::Down ? 1U : 0U;
}

[[nodiscard]] std::size_t side_index(mm::QuoteSide side) noexcept {
    return side == mm::QuoteSide::Ask ? 1U : 0U;
}

[[nodiscard]] std::size_t purpose_index(mm::QuoteIntentType type) noexcept {
    if (type == mm::QuoteIntentType::ForcedUnwind) {
        return 2;
    }
    if (type == mm::QuoteIntentType::PassiveUnwind) {
        return 1;
    }
    return 0;
}

[[nodiscard]] std::int64_t bid_cross_distance(
    std::int64_t bid_tick,
    std::int64_t best_ask_tick
) noexcept {
    return bid_tick >= best_ask_tick ? bid_tick - best_ask_tick + 1 : 0;
}

[[nodiscard]] std::int64_t ask_cross_distance(
    std::int64_t ask_tick,
    std::int64_t best_bid_tick
) noexcept {
    return ask_tick <= best_bid_tick ? best_bid_tick - ask_tick + 1 : 0;
}

[[nodiscard]] bool post_only_valid(
    mm::QuoteSide side,
    std::int64_t price_tick,
    std::int64_t best_bid_tick,
    std::int64_t best_ask_tick
) noexcept {
    return side == mm::QuoteSide::Bid ? price_tick < best_ask_tick
                                      : price_tick > best_bid_tick;
}

void observe_quote_validity(
    const mm::QuoteLeg& leg,
    const mm::QuoteIntent& quote,
    std::int64_t best_bid_tick,
    std::int64_t best_ask_tick,
    bool approved,
    Metrics* metrics
) {
    const auto opening = !leg.risk_reducing;
    if (opening && leg.edge_to_fair_tick <= 0) {
        ++metrics->quote_validity.negative_opening_edge_quotes;
    }
    if (leg.side == mm::QuoteSide::Bid) {
        const auto cross = bid_cross_distance(leg.price_tick, best_ask_tick);
        metrics->quote_validity.max_bid_cross_tick =
            std::max(metrics->quote_validity.max_bid_cross_tick, cross);
        if (approved && cross > 0) {
            ++metrics->quote_validity.approved_crossing_quotes;
        }
    } else {
        const auto cross = ask_cross_distance(leg.price_tick, best_bid_tick);
        metrics->quote_validity.max_ask_cross_tick =
            std::max(metrics->quote_validity.max_ask_cross_tick, cross);
        if (approved && cross > 0) {
            ++metrics->quote_validity.approved_crossing_quotes;
        }
    }
    (void)quote;
}

void add_regime_fill(
    Metrics* metrics,
    std::int64_t tau_ms,
    std::int64_t z_score_bps,
    std::int64_t spread_capture_tick
) {
    const auto tau_sec = tau_ms / 1000;
    std::size_t tau_bucket = 3;
    if (tau_sec > 180) {
        tau_bucket = 0;
    } else if (tau_sec > 60) {
        tau_bucket = 1;
    } else if (tau_sec > 30) {
        tau_bucket = 2;
    }
    ++metrics->fills_by_regime[tau_bucket].fills;
    metrics->fills_by_regime[tau_bucket].spread_capture_tick +=
        spread_capture_tick;

    std::size_t z_bucket = 6;
    if (z_score_bps < 5'000) {
        z_bucket = 4;
    } else if (z_score_bps < 10'000) {
        z_bucket = 5;
    }
    ++metrics->fills_by_regime[z_bucket].fills;
    metrics->fills_by_regime[z_bucket].spread_capture_tick +=
        spread_capture_tick;
}

[[nodiscard]] std::int64_t projected_canonical_position(
    const mm::QuoteIntent& quote,
    std::int64_t current_canonical
) noexcept {
    auto projected = current_canonical;
    if (quote.has_bid) {
        const auto candidate = current_canonical + canonical_delta(
            quote.product_asset_direction,
            mm::QuoteSide::Bid,
            quote.bid.quantity_lots
        );
        if (std::llabs(candidate) > std::llabs(projected)) {
            projected = candidate;
        }
    }
    if (quote.has_ask) {
        const auto candidate = current_canonical + canonical_delta(
            quote.product_asset_direction,
            mm::QuoteSide::Ask,
            quote.ask.quantity_lots
        );
        if (std::llabs(candidate) > std::llabs(projected)) {
            projected = candidate;
        }
    }
    return projected;
}

void write_event_header(std::ofstream* out) {
    *out << "second,round,asset,spot,fair_tick,book_mid_tick,position_lots,"
            "canonical_lots,event,decision,reason,bid_tick,ask_tick,fill_qty,"
            "fill_price_tick,cash_tick,best_bid_tick,best_ask_tick,quote_side,"
            "token_side,canonical_side,canonical_delta_if_filled,"
            "quote_purpose,tau_sec,z_score,is_opening,is_unwind,"
            "post_only_valid,cross_distance_tick,edge_to_fair_tick,"
            "edge_to_book_tick\n";
}

void write_event(
    std::ofstream* out,
    std::int64_t second,
    std::int64_t round,
    const TokenRuntime& token,
    double spot,
    std::int64_t fair_tick,
    std::int64_t book_mid_tick,
    std::int64_t canonical_lots,
    const std::string& event,
    const std::string& decision,
    const std::string& reason,
    std::int64_t bid_tick = 0,
    std::int64_t ask_tick = 0,
    std::int64_t fill_qty = 0,
    std::int64_t fill_price_tick = 0,
    std::int64_t best_bid_tick = 0,
    std::int64_t best_ask_tick = 0,
    const std::string& quote_side = "",
    const std::string& token_side = "",
    const std::string& canonical_side = "",
    std::int64_t canonical_delta_if_filled = 0,
    const std::string& quote_purpose = "",
    std::int64_t tau_sec = 0,
    std::int64_t z_score_bps = 0,
    bool is_opening = false,
    bool is_unwind = false,
    bool post_only_ok = false,
    std::int64_t cross_distance_tick = 0,
    std::int64_t edge_to_fair_tick = 0,
    std::int64_t edge_to_book_tick = 0
) {
    *out << second << ',' << round << ',' << token.asset_id << ','
         << std::fixed << std::setprecision(2) << spot << ','
         << fair_tick << ',' << book_mid_tick << ','
         << token.position_lots << ',' << canonical_lots << ','
         << event << ',' << decision << ",\"" << reason << "\","
         << bid_tick << ',' << ask_tick << ',' << fill_qty << ','
         << fill_price_tick << ',' << token.cash_tick << ','
         << best_bid_tick << ',' << best_ask_tick << ','
         << quote_side << ',' << token_side << ',' << canonical_side << ','
         << canonical_delta_if_filled << ',' << quote_purpose << ','
         << tau_sec << ',' << z_score_bps << ','
         << (is_opening ? 1 : 0) << ',' << (is_unwind ? 1 : 0) << ','
         << (post_only_ok ? 1 : 0) << ',' << cross_distance_tick << ','
         << edge_to_fair_tick << ',' << edge_to_book_tick << '\n';
}

void write_quote_leg_event(
    std::ofstream* out,
    std::int64_t second,
    std::int64_t round,
    const TokenRuntime& token,
    double spot,
    std::int64_t fair_tick,
    std::int64_t book_mid_tick,
    std::int64_t canonical_lots,
    const mm::QuoteIntent& quote,
    const mm::QuoteLeg& leg,
    std::int64_t best_bid_tick,
    std::int64_t best_ask_tick,
    const std::string& decision,
    const std::string& reason
) {
    const auto delta = canonical_delta(
        quote.product_asset_direction,
        leg.side,
        leg.quantity_lots
    );
    const auto cross = leg.side == mm::QuoteSide::Bid
        ? bid_cross_distance(leg.price_tick, best_ask_tick)
        : ask_cross_distance(leg.price_tick, best_bid_tick);
    const auto edge_to_book = leg.side == mm::QuoteSide::Bid
        ? leg.price_tick - best_bid_tick
        : best_ask_tick - leg.price_tick;
    write_event(
        out,
        second,
        round,
        token,
        spot,
        fair_tick,
        book_mid_tick,
        canonical_lots,
        "quote",
        decision,
        reason,
        leg.side == mm::QuoteSide::Bid ? leg.price_tick : 0,
        leg.side == mm::QuoteSide::Ask ? leg.price_tick : 0,
        0,
        0,
        best_bid_tick,
        best_ask_tick,
        quote_side_name(leg.side),
        token_side_name(quote.product_asset_direction),
        canonical_side_name(delta),
        delta,
        quote_purpose_name(quote.type),
        static_cast<std::int64_t>(quote.product_tau_ms / 1000),
        quote.product_z_score_bps,
        !leg.risk_reducing,
        leg.risk_reducing || quote.type == mm::QuoteIntentType::PassiveUnwind ||
            quote.type == mm::QuoteIntentType::ForcedUnwind,
        post_only_valid(leg.side, leg.price_tick, best_bid_tick, best_ask_tick),
        cross,
        leg.edge_to_fair_tick,
        edge_to_book
    );
}

void maybe_fill_active_quote(
    TokenRuntime* token,
    std::int64_t market_mid_tick,
    std::int64_t fair_tick,
    Metrics* metrics,
    std::ofstream* events,
    std::int64_t second,
    std::int64_t round,
    double spot,
    std::int64_t canonical_lots
) {
    if (!token || !token->active_quote) {
        return;
    }
    auto& active = *token->active_quote;
    auto& quote = active.approved;
    auto fill_side = std::optional<mm::QuoteSide>{};
    std::int64_t fill_price_tick = 0;
    std::int64_t fill_qty_lots = 0;
    if (quote.has_bid && market_mid_tick <= quote.bid.price_tick + 1'000) {
        fill_side = mm::QuoteSide::Bid;
        fill_price_tick = quote.bid.price_tick;
        fill_qty_lots = quote.bid.quantity_lots;
    } else if (quote.has_ask && market_mid_tick >= quote.ask.price_tick - 1'000) {
        fill_side = mm::QuoteSide::Ask;
        fill_price_tick = quote.ask.price_tick;
        fill_qty_lots = quote.ask.quantity_lots;
    }
    if (!fill_side || fill_qty_lots <= 0) {
        return;
    }

    ++metrics->fills;
    metrics->traded_lots += fill_qty_lots;
    metrics->traded_notional_tick += fill_price_tick * fill_qty_lots;
    std::int64_t fill_spread_capture_tick = 0;
    if (*fill_side == mm::QuoteSide::Bid) {
        ++metrics->bid_fills;
        token->position_lots += fill_qty_lots;
        token->cash_tick -= fill_price_tick * fill_qty_lots;
        fill_spread_capture_tick = (fair_tick - fill_price_tick) * fill_qty_lots;
    } else {
        ++metrics->ask_fills;
        token->position_lots -= fill_qty_lots;
        token->cash_tick += fill_price_tick * fill_qty_lots;
        fill_spread_capture_tick = (fill_price_tick - fair_tick) * fill_qty_lots;
    }
    metrics->spread_capture_tick += fill_spread_capture_tick;
    metrics->spread_capture_by_token[token_index(active.token_direction)] +=
        fill_spread_capture_tick;
    metrics->spread_capture_by_side[side_index(*fill_side)] +=
        fill_spread_capture_tick;
    metrics->spread_capture_by_purpose[purpose_index(active.type)] +=
        fill_spread_capture_tick;
    add_regime_fill(
        metrics,
        active.tau_ms,
        active.z_score_bps,
        fill_spread_capture_tick
    );
    const auto delta = canonical_delta(
        active.token_direction,
        *fill_side,
        fill_qty_lots
    );
    const auto edge_to_fair = *fill_side == mm::QuoteSide::Bid
        ? fair_tick - fill_price_tick
        : fill_price_tick - fair_tick;
    const auto edge_to_book = *fill_side == mm::QuoteSide::Bid
        ? fill_price_tick - active.best_bid_tick
        : active.best_ask_tick - fill_price_tick;
    write_event(
        events,
        second,
        round,
        *token,
        spot,
        fair_tick,
        market_mid_tick,
        canonical_lots,
        *fill_side == mm::QuoteSide::Bid ? "fill_bid" : "fill_ask",
        "filled",
        "synthetic_taker_cross",
        0,
        0,
        fill_qty_lots,
        fill_price_tick,
        active.best_bid_tick,
        active.best_ask_tick,
        quote_side_name(*fill_side),
        token_side_name(active.token_direction),
        canonical_side_name(delta),
        delta,
        quote_purpose_name(active.type),
        active.tau_ms / 1000,
        active.z_score_bps,
        active.type != mm::QuoteIntentType::PassiveUnwind &&
            active.type != mm::QuoteIntentType::ForcedUnwind,
        active.type == mm::QuoteIntentType::PassiveUnwind ||
            active.type == mm::QuoteIntentType::ForcedUnwind,
        post_only_valid(
            *fill_side,
            fill_price_tick,
            active.best_bid_tick,
            active.best_ask_tick
        ),
        *fill_side == mm::QuoteSide::Bid
            ? bid_cross_distance(fill_price_tick, active.best_ask_tick)
            : ask_cross_distance(fill_price_tick, active.best_bid_tick),
        edge_to_fair,
        edge_to_book
    );
    token->engine.remove_active_quote(token->asset_index);
    token->active_quote.reset();
}

void maybe_fill_active_quote_from_real_trades(
    TokenRuntime* token,
    std::int64_t buy_trade_tick,
    std::int64_t sell_trade_tick,
    std::int64_t fair_tick,
    Metrics* metrics,
    std::ofstream* events,
    std::int64_t second,
    std::int64_t round,
    double spot,
    std::int64_t book_mid_tick,
    std::int64_t canonical_lots
) {
    if (!token || !token->active_quote) {
        return;
    }
    auto& active = *token->active_quote;
    auto& quote = active.approved;
    auto fill_side = std::optional<mm::QuoteSide>{};
    std::int64_t fill_price_tick = 0;
    std::int64_t fill_qty_lots = 0;
    if (quote.has_ask && buy_trade_tick > 0 &&
        buy_trade_tick >= quote.ask.price_tick) {
        fill_side = mm::QuoteSide::Ask;
        fill_price_tick = quote.ask.price_tick;
        fill_qty_lots = quote.ask.quantity_lots;
    } else if (quote.has_bid && sell_trade_tick > 0 &&
               sell_trade_tick <= quote.bid.price_tick) {
        fill_side = mm::QuoteSide::Bid;
        fill_price_tick = quote.bid.price_tick;
        fill_qty_lots = quote.bid.quantity_lots;
    }
    if (!fill_side || fill_qty_lots <= 0) {
        return;
    }

    ++metrics->fills;
    metrics->traded_lots += fill_qty_lots;
    metrics->traded_notional_tick += fill_price_tick * fill_qty_lots;
    std::int64_t fill_spread_capture_tick = 0;
    if (*fill_side == mm::QuoteSide::Bid) {
        ++metrics->bid_fills;
        token->position_lots += fill_qty_lots;
        token->cash_tick -= fill_price_tick * fill_qty_lots;
        fill_spread_capture_tick = (fair_tick - fill_price_tick) * fill_qty_lots;
    } else {
        ++metrics->ask_fills;
        token->position_lots -= fill_qty_lots;
        token->cash_tick += fill_price_tick * fill_qty_lots;
        fill_spread_capture_tick = (fill_price_tick - fair_tick) * fill_qty_lots;
    }
    metrics->spread_capture_tick += fill_spread_capture_tick;
    metrics->spread_capture_by_token[token_index(active.token_direction)] +=
        fill_spread_capture_tick;
    metrics->spread_capture_by_side[side_index(*fill_side)] +=
        fill_spread_capture_tick;
    metrics->spread_capture_by_purpose[purpose_index(active.type)] +=
        fill_spread_capture_tick;
    add_regime_fill(
        metrics,
        active.tau_ms,
        active.z_score_bps,
        fill_spread_capture_tick
    );
    const auto delta = canonical_delta(
        active.token_direction,
        *fill_side,
        fill_qty_lots
    );
    const auto edge_to_fair = *fill_side == mm::QuoteSide::Bid
        ? fair_tick - fill_price_tick
        : fill_price_tick - fair_tick;
    const auto edge_to_book = *fill_side == mm::QuoteSide::Bid
        ? fill_price_tick - active.best_bid_tick
        : active.best_ask_tick - fill_price_tick;
    write_event(
        events,
        second,
        round,
        *token,
        spot,
        fair_tick,
        book_mid_tick,
        canonical_lots,
        *fill_side == mm::QuoteSide::Bid ? "real_fill_bid" : "real_fill_ask",
        "filled",
        "polymarket_trade_cross",
        0,
        0,
        fill_qty_lots,
        fill_price_tick,
        active.best_bid_tick,
        active.best_ask_tick,
        quote_side_name(*fill_side),
        token_side_name(active.token_direction),
        canonical_side_name(delta),
        delta,
        quote_purpose_name(active.type),
        active.tau_ms / 1000,
        active.z_score_bps,
        active.type != mm::QuoteIntentType::PassiveUnwind &&
            active.type != mm::QuoteIntentType::ForcedUnwind,
        active.type == mm::QuoteIntentType::PassiveUnwind ||
            active.type == mm::QuoteIntentType::ForcedUnwind,
        post_only_valid(
            *fill_side,
            fill_price_tick,
            active.best_bid_tick,
            active.best_ask_tick
        ),
        *fill_side == mm::QuoteSide::Bid
            ? bid_cross_distance(fill_price_tick, active.best_ask_tick)
            : ask_cross_distance(fill_price_tick, active.best_bid_tick),
        edge_to_fair,
        edge_to_book
    );
    token->engine.remove_active_quote(token->asset_index);
    token->active_quote.reset();
}

void settle_round(
    TokenRuntime* up,
    TokenRuntime* down,
    bool up_wins,
    Metrics* metrics
) {
    const auto up_payout = up_wins ? mm::kPriceOneTick : 0;
    const auto down_payout = up_wins ? 0 : mm::kPriceOneTick;
    up->cash_tick += up->position_lots * up_payout;
    down->cash_tick += down->position_lots * down_payout;
    up->position_lots = 0;
    down->position_lots = 0;
    up->active_quote.reset();
    down->active_quote.reset();
    up->engine.remove_active_quote(up->asset_index);
    down->engine.remove_active_quote(down->asset_index);
    metrics->settlement_pnl_tick = up->cash_tick + down->cash_tick;
}

void run_token_step(
    TokenRuntime* token,
    const TokenRuntime& up,
    const TokenRuntime& down,
    const updown::UpDown5mMarketContext& base_context,
    const state::MarketDepthView& depth,
    const state::MarketDepthView& complement_depth,
    std::int64_t book_mid_tick,
    const updown::UpDown5mFairOutput& fair,
    const updown::UpDown5mQuoteController& controller,
    const risk::QuoteRiskPolicy& policy,
    Metrics* metrics,
    std::ofstream* events,
    std::int64_t second,
    std::int64_t round,
    bool synthetic_fill,
    std::int64_t buy_trade_tick,
    std::int64_t sell_trade_tick
) {
    auto context = base_context;
    context.asset_direction = token->direction;
    const auto plan = controller.build_plan(context, token->position_lots);
    if (plan.ok) {
        ++metrics->plans_ok;
    }
    if (plan.opening_allowed) {
        ++metrics->opening_allowed;
    }
    if (plan.cancel_all_quotes) {
        ++metrics->controller_cancels;
    }

    const auto now_ns = static_cast<std::uint64_t>(second) * kNsPerSecond;
    if (token->active_quote &&
        token->active_quote->approved.expires_at_ns <= now_ns) {
        token->active_quote.reset();
        token->engine.remove_active_quote(token->asset_index);
    }

    mm::MarketMakingInput input{
        .market_id = context.market_id,
        .asset_id = token->asset_id,
        .market_index = 5,
        .asset_index = token->asset_index,
        .depth = &depth,
        .complement_depth = &complement_depth,
        .current_position_lots = token->position_lots,
        .now_ns = now_ns
    };
    plan.apply_to(&input);
    input.canonical_yes_position_lots = canonical_position(up, down);

    const auto result = token->engine.on_market_update(input);
    metrics->snapshots += result.snapshots_seen;
    metrics->quote_intents += result.quote_count;
    metrics->cancel_intents += result.cancel_count;
    if (result.rejected_no_quote > 0) {
        ++metrics->no_quote;
    }
    if (result.cancel_count > 0) {
        token->active_quote.reset();
    }

    for (std::uint16_t i = 0; i < result.quote_count; ++i) {
        const auto& quote = result.quotes[i];
        const auto current_canonical = canonical_position(up, down);
        const auto risk_result = risk::QuoteRiskEvaluator{}.evaluate(
            risk::QuoteRiskInput{
                .quote = &quote,
                .depth = &depth,
                .policy = &policy,
                .current_position_lots = token->position_lots,
                .current_canonical_yes_position_lots = current_canonical,
                .projected_canonical_yes_position_lots =
                    projected_canonical_position(quote, current_canonical),
                .spot_age_ms = static_cast<std::int64_t>(context.oracle_age_ms),
                .vol_age_ms = 1'000,
                .external_confidence_bps = 10'000,
                .active_quotes_for_asset = token->active_quote ? 1U : 0U,
                .last_replace_ts_ns = 0,
                .now_ns = now_ns
            }
        );
        const auto idx = static_cast<std::size_t>(risk_result.decision.decision);
        if (idx < metrics->risk_counts.size()) {
            ++metrics->risk_counts[idx];
        }
        const auto approved =
            risk_result.decision.decision ==
                risk::QuoteRiskDecisionType::Approve &&
            risk_result.approved_quote.has_value();
        if (quote.has_bid) {
            observe_quote_validity(
                quote.bid,
                quote,
                depth.bids[0].price_tick,
                depth.asks[0].price_tick,
                approved,
                metrics
            );
        }
        if (quote.has_ask) {
            observe_quote_validity(
                quote.ask,
                quote,
                depth.bids[0].price_tick,
                depth.asks[0].price_tick,
                approved,
                metrics
            );
        }

        if (risk_result.decision.decision ==
            risk::QuoteRiskDecisionType::RejectPostOnlyCross) {
            ++metrics->quote_validity.post_only_cross_rejects;
        }

        const auto decision_name = approved
            ? std::string("approve")
            : std::string(
                  risk::quote_risk_decision_type_name(
                      risk_result.decision.decision
                  )
              );
        const auto reason = approved
            ? quote.reason
            : risk_result.decision.reason;
        if (quote.has_bid) {
            write_quote_leg_event(
                events,
                second,
                round,
                *token,
                context.current_spot,
                fair.asset_fair_tick,
                book_mid_tick,
                current_canonical,
                quote,
                quote.bid,
                depth.bids[0].price_tick,
                depth.asks[0].price_tick,
                decision_name,
                reason
            );
        }
        if (quote.has_ask) {
            write_quote_leg_event(
                events,
                second,
                round,
                *token,
                context.current_spot,
                fair.asset_fair_tick,
                book_mid_tick,
                current_canonical,
                quote,
                quote.ask,
                depth.bids[0].price_tick,
                depth.asks[0].price_tick,
                decision_name,
                reason
            );
        }

        if (approved) {
            ++metrics->risk_approved;
            token->active_quote = TokenRuntime::ActiveReplayQuote{
                .approved = *risk_result.approved_quote,
                .type = quote.type,
                .token_direction = quote.product_asset_direction,
                .tau_ms = static_cast<std::int64_t>(quote.product_tau_ms),
                .z_score_bps = quote.product_z_score_bps,
                .fair_at_quote_tick = quote.fair_value_tick,
                .best_bid_tick = depth.bids[0].price_tick,
                .best_ask_tick = depth.asks[0].price_tick
            };
        } else {
            ++metrics->risk_rejected;
            token->active_quote.reset();
            token->engine.remove_active_quote(token->asset_index);
        }
    }

    if (synthetic_fill) {
        maybe_fill_active_quote(
            token,
            book_mid_tick,
            fair.asset_fair_tick,
            metrics,
            events,
            second,
            round,
            context.current_spot,
            canonical_position(up, down)
        );
    } else {
        maybe_fill_active_quote_from_real_trades(
            token,
            buy_trade_tick,
            sell_trade_tick,
            fair.asset_fair_tick,
            metrics,
            events,
            second,
            round,
            context.current_spot,
            book_mid_tick,
            canonical_position(up, down)
        );
    }
}

void write_summary(
    const std::filesystem::path& path,
    const Metrics& metrics,
    const TokenRuntime& up,
    const TokenRuntime& down,
    const std::string& mode,
    const std::string& note
) {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("failed to write summary: " + path.string());
    }
    const auto final_pnl = up.cash_tick + down.cash_tick;
    out << "{\n";
    out << "  \"mode\": \"" << mode << "\",\n";
    out << "  \"note\": \"" << note << "\",\n";
    out << "  \"duration_seconds\": " << kTotalSeconds << ",\n";
    out << "  \"rounds\": " << (kTotalSeconds / kRoundSeconds) << ",\n";
    out << "  \"snapshots\": " << metrics.snapshots << ",\n";
    out << "  \"plans_ok\": " << metrics.plans_ok << ",\n";
    out << "  \"opening_allowed_steps\": " << metrics.opening_allowed << ",\n";
    out << "  \"controller_cancel_steps\": " << metrics.controller_cancels << ",\n";
    out << "  \"no_quote_steps\": " << metrics.no_quote << ",\n";
    out << "  \"quote_intents\": " << metrics.quote_intents << ",\n";
    out << "  \"cancel_intents\": " << metrics.cancel_intents << ",\n";
    out << "  \"risk_approved\": " << metrics.risk_approved << ",\n";
    out << "  \"risk_rejected\": " << metrics.risk_rejected << ",\n";
    out << "  \"fills\": " << metrics.fills << ",\n";
    out << "  \"bid_fills\": " << metrics.bid_fills << ",\n";
    out << "  \"ask_fills\": " << metrics.ask_fills << ",\n";
    out << "  \"traded_lots\": " << metrics.traded_lots << ",\n";
    out << "  \"traded_notional_tick\": " << metrics.traded_notional_tick << ",\n";
    out << "  \"spread_capture_tick\": " << metrics.spread_capture_tick << ",\n";
    out << "  \"final_pnl_tick\": " << final_pnl << ",\n";
    out << "  \"max_abs_canonical_lots\": " << metrics.max_abs_canonical_lots << ",\n";
    out << "  \"up_cash_tick\": " << up.cash_tick << ",\n";
    out << "  \"down_cash_tick\": " << down.cash_tick << ",\n";
    out << "  \"risk_decisions\": {\n";
    bool first = true;
    for (std::size_t i = 0; i < metrics.risk_counts.size(); ++i) {
        if (metrics.risk_counts[i] == 0) {
            continue;
        }
        if (!first) {
            out << ",\n";
        }
        first = false;
        out << "    \""
            << risk::quote_risk_decision_type_name(
                   static_cast<risk::QuoteRiskDecisionType>(i)
               )
            << "\": " << metrics.risk_counts[i];
    }
    out << "\n  },\n";
    out << "  \"quote_validity\": {\n";
    out << "    \"post_only_cross_rejects\": "
        << metrics.quote_validity.post_only_cross_rejects << ",\n";
    out << "    \"approved_crossing_quotes\": "
        << metrics.quote_validity.approved_crossing_quotes << ",\n";
    out << "    \"negative_opening_edge_quotes\": "
        << metrics.quote_validity.negative_opening_edge_quotes << ",\n";
    out << "    \"max_bid_cross_tick\": "
        << metrics.quote_validity.max_bid_cross_tick << ",\n";
    out << "    \"max_ask_cross_tick\": "
        << metrics.quote_validity.max_ask_cross_tick << "\n";
    out << "  },\n";
    out << "  \"spread_capture_by_token\": {\n";
    out << "    \"up\": " << metrics.spread_capture_by_token[0] << ",\n";
    out << "    \"down\": " << metrics.spread_capture_by_token[1] << "\n";
    out << "  },\n";
    out << "  \"spread_capture_by_side\": {\n";
    out << "    \"bid\": " << metrics.spread_capture_by_side[0] << ",\n";
    out << "    \"ask\": " << metrics.spread_capture_by_side[1] << "\n";
    out << "  },\n";
    out << "  \"spread_capture_by_purpose\": {\n";
    out << "    \"opening\": " << metrics.spread_capture_by_purpose[0] << ",\n";
    out << "    \"passive_unwind\": " << metrics.spread_capture_by_purpose[1] << ",\n";
    out << "    \"forced_unwind\": " << metrics.spread_capture_by_purpose[2] << "\n";
    out << "  },\n";
    out << "  \"fills_by_regime\": {\n";
    out << "    \"tau_gt_180\": {\"fills\": "
        << metrics.fills_by_regime[0].fills
        << ", \"spread_capture_tick\": "
        << metrics.fills_by_regime[0].spread_capture_tick << "},\n";
    out << "    \"tau_60_180\": {\"fills\": "
        << metrics.fills_by_regime[1].fills
        << ", \"spread_capture_tick\": "
        << metrics.fills_by_regime[1].spread_capture_tick << "},\n";
    out << "    \"tau_30_60\": {\"fills\": "
        << metrics.fills_by_regime[2].fills
        << ", \"spread_capture_tick\": "
        << metrics.fills_by_regime[2].spread_capture_tick << "},\n";
    out << "    \"tau_lt_30\": {\"fills\": "
        << metrics.fills_by_regime[3].fills
        << ", \"spread_capture_tick\": "
        << metrics.fills_by_regime[3].spread_capture_tick << "},\n";
    out << "    \"z_0_0p5\": {\"fills\": "
        << metrics.fills_by_regime[4].fills
        << ", \"spread_capture_tick\": "
        << metrics.fills_by_regime[4].spread_capture_tick << "},\n";
    out << "    \"z_0p5_1\": {\"fills\": "
        << metrics.fills_by_regime[5].fills
        << ", \"spread_capture_tick\": "
        << metrics.fills_by_regime[5].spread_capture_tick << "},\n";
    out << "    \"z_gt_1\": {\"fills\": "
        << metrics.fills_by_regime[6].fills
        << ", \"spread_capture_tick\": "
        << metrics.fills_by_regime[6].spread_capture_tick << "}\n";
    out << "  }\n";
    out << "}\n";
}

[[nodiscard]] std::vector<std::string> split_csv_line(
    const std::string& line
) {
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, ',')) {
        out.push_back(cell);
    }
    return out;
}

[[nodiscard]] std::int64_t parse_i64(const std::string& value) {
    if (value.empty()) {
        return 0;
    }
    return std::stoll(value);
}

[[nodiscard]] double parse_double(const std::string& value) {
    if (value.empty()) {
        return 0.0;
    }
    return std::stod(value);
}

[[nodiscard]] RealInputRow parse_real_input_row(
    const std::vector<std::string>& columns
) {
    if (columns.size() < 19) {
        throw std::runtime_error("real input csv row has too few columns");
    }
    RealInputRow row;
    row.second = parse_i64(columns[0]);
    row.round = parse_i64(columns[1]);
    row.market_slug = columns[2];
    row.condition_id = columns[3];
    row.up_token_id = columns[4];
    row.down_token_id = columns[5];
    row.round_start_ms = parse_i64(columns[6]);
    row.round_expiry_ms = parse_i64(columns[7]);
    row.start_spot = parse_double(columns[8]);
    row.current_spot = parse_double(columns[9]);
    row.sigma_annualized = parse_double(columns[10]);
    row.up_bid_tick = parse_i64(columns[11]);
    row.up_ask_tick = parse_i64(columns[12]);
    row.down_bid_tick = parse_i64(columns[13]);
    row.down_ask_tick = parse_i64(columns[14]);
    row.up_buy_trade_tick = parse_i64(columns[15]);
    row.up_sell_trade_tick = parse_i64(columns[16]);
    row.down_buy_trade_tick = parse_i64(columns[17]);
    row.down_sell_trade_tick = parse_i64(columns[18]);
    return row;
}

[[nodiscard]] std::vector<RealInputRow> read_real_input_csv(
    const std::filesystem::path& csv_path
) {
    std::ifstream in(csv_path);
    if (!in) {
        throw std::runtime_error("failed to read real input csv: " + csv_path.string());
    }
    std::vector<RealInputRow> rows;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        if (header) {
            header = false;
            continue;
        }
        rows.push_back(parse_real_input_row(split_csv_line(line)));
    }
    if (rows.empty()) {
        throw std::runtime_error("real input csv had no rows");
    }
    return rows;
}

int run_real_csv_backtest(
    const std::filesystem::path& csv_path,
    const std::filesystem::path& output_dir,
    std::int64_t conservative_book_half_spread_tick
) {
    const auto rows = read_real_input_csv(csv_path);
    std::filesystem::create_directories(output_dir);

    const auto up_config = market_making_config(60'501);
    const auto down_config = market_making_config(60'502);
    TokenRuntime up{
        .direction = mm::ProductDirection::Up,
        .asset_id = rows.front().up_token_id,
        .asset_index = kUpAssetIndex,
        .engine = mm::MarketMakingEngine(up_config)
    };
    TokenRuntime down{
        .direction = mm::ProductDirection::Down,
        .asset_id = rows.front().down_token_id,
        .asset_index = kDownAssetIndex,
        .engine = mm::MarketMakingEngine(down_config)
    };

    updown::UpDown5mQuoteControllerConfig controller_config;
    controller_config.base_max_canonical_lots = 60;
    controller_config.base_half_spread_tick = 15'000;
    controller_config.allow_directional_inventory = false;
    const updown::UpDown5mQuoteController controller(controller_config);
    const updown::UpDown5mFairProvider fair_provider;
    const auto policy = risk_policy();

    std::ofstream events(output_dir / "events.csv");
    if (!events) {
        throw std::runtime_error("failed to write real events.csv");
    }
    write_event_header(&events);

    Metrics metrics;
    auto active_round = rows.front().round;
    auto active_round_start_spot = rows.front().start_spot;
    auto last_spot = rows.front().current_spot;

    for (const auto& row : rows) {
        if (row.round != active_round) {
            settle_round(
                &up,
                &down,
                last_spot >= active_round_start_spot,
                &metrics
            );
            active_round = row.round;
            active_round_start_spot = row.start_spot;
            up.asset_id = row.up_token_id;
            down.asset_id = row.down_token_id;
        }
        ++metrics.seconds;
        last_spot = row.current_spot;

        const auto up_mid_tick = (row.up_bid_tick + row.up_ask_tick) / 2;
        const auto down_mid_tick = (row.down_bid_tick + row.down_ask_tick) / 2;

        updown::UpDown5mMarketContext context{
            .market_id = row.market_slug,
            .up_token_id = row.up_token_id,
            .down_token_id = row.down_token_id,
            .asset_direction = mm::ProductDirection::Up,
            .round_start_ms = row.round_start_ms,
            .round_expiry_ms = row.round_expiry_ms,
            .now_ms = row.second * 1'000,
            .start_spot = row.start_spot,
            .current_spot = row.current_spot,
            .sigma_annualized = row.sigma_annualized,
            .oracle_age_ms = 100.0,
            .polymarket_up_mid =
                static_cast<double>(up_mid_tick) /
                static_cast<double>(mm::kPriceOneTick),
            .polymarket_down_mid =
                static_cast<double>(down_mid_tick) /
                static_cast<double>(mm::kPriceOneTick),
            .settlement_source_ok = true,
            .book_ok = true
        };

        context.asset_direction = mm::ProductDirection::Up;
        const auto up_fair = fair_provider.compute(context);
        context.asset_direction = mm::ProductDirection::Down;
        const auto down_fair = fair_provider.compute(context);

        const auto up_book = reconstructed_book_ticks(
            row.up_bid_tick,
            row.up_ask_tick,
            up_mid_tick,
            up_fair.z_score_bps,
            row.second,
            conservative_book_half_spread_tick
        );
        const auto down_book = reconstructed_book_ticks(
            row.down_bid_tick,
            row.down_ask_tick,
            down_mid_tick,
            down_fair.z_score_bps,
            row.second,
            conservative_book_half_spread_tick
        );
        const auto now_ns =
            static_cast<std::uint64_t>(row.second) * kNsPerSecond;
        const auto up_depth = make_depth_from_bid_ask(
            up_book.bid_tick,
            up_book.ask_tick,
            up.asset_index,
            static_cast<std::uint64_t>(row.second * 2 + 1),
            now_ns
        );
        const auto down_depth = make_depth_from_bid_ask(
            down_book.bid_tick,
            down_book.ask_tick,
            down.asset_index,
            static_cast<std::uint64_t>(row.second * 2 + 2),
            now_ns
        );

        context.asset_direction = mm::ProductDirection::Up;
        run_token_step(
            &up,
            up,
            down,
            context,
            up_depth,
            down_depth,
            up_mid_tick,
            up_fair,
            controller,
            policy,
            &metrics,
            &events,
            row.second,
            row.round,
            false,
            row.up_buy_trade_tick,
            row.up_sell_trade_tick
        );

        context.asset_direction = mm::ProductDirection::Down;
        run_token_step(
            &down,
            up,
            down,
            context,
            down_depth,
            up_depth,
            down_mid_tick,
            down_fair,
            controller,
            policy,
            &metrics,
            &events,
            row.second,
            row.round,
            false,
            row.down_buy_trade_tick,
            row.down_sell_trade_tick
        );

        metrics.max_abs_canonical_lots = std::max<std::int64_t>(
            metrics.max_abs_canonical_lots,
            std::llabs(canonical_position(up, down))
        );
    }
    settle_round(&up, &down, last_spot >= active_round_start_spot, &metrics);

    const auto note = conservative_book_half_spread_tick > 0
        ? "Uses Polymarket Gamma/CLOB/data-api market data and Coinbase BTC-USD candles. Historical CLOB L2 snapshots are not public here, so per-second books are reconstructed from real Polymarket price history/trades with a dynamic conservative top-of-book spread. conservative_base_half_spread_tick=" +
              std::to_string(conservative_book_half_spread_tick)
        : "Uses Polymarket Gamma/CLOB/data-api market data and Coinbase BTC-USD candles. Historical CLOB L2 snapshots are not public here, so per-second books are reconstructed from real Polymarket price history/trades with a one-cent top-of-book spread.";
    write_summary(
        output_dir / "summary.json",
        metrics,
        up,
        down,
        "real_market_api_30m_replay",
        note
    );

    std::cout << "real market 30m replay complete\n";
    std::cout << "summary=" << (output_dir / "summary.json").string() << "\n";
    std::cout << "events=" << (output_dir / "events.csv").string() << "\n";
    std::cout << "quotes=" << metrics.quote_intents
              << " approved=" << metrics.risk_approved
              << " rejected=" << metrics.risk_rejected
              << " fills=" << metrics.fills
              << " pnl_tick=" << (up.cash_tick + down.cash_tick)
              << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--csv") {
        const auto csv_path = std::filesystem::path{argv[2]};
        auto output_dir =
            std::filesystem::path{"runs/up_down_5m_backtest_30m_real_market"};
        std::int64_t conservative_book_half_spread_tick = 0;
        for (int i = 3; i < argc; ++i) {
            const auto arg = std::string(argv[i]);
            if (arg == "--out-dir" && i + 1 < argc) {
                output_dir = std::filesystem::path{argv[++i]};
            } else if (arg == "--conservative-book-half-spread-tick" &&
                       i + 1 < argc) {
                conservative_book_half_spread_tick = std::stoll(argv[++i]);
            }
        }
        return run_real_csv_backtest(
            csv_path,
            output_dir,
            conservative_book_half_spread_tick
        );
    }

    const auto output_dir =
        std::filesystem::path{"runs/up_down_5m_backtest_30m_synthetic"};
    std::filesystem::create_directories(output_dir);

    const auto up_config = market_making_config(50'501);
    const auto down_config = market_making_config(50'502);
    TokenRuntime up{
        .direction = mm::ProductDirection::Up,
        .asset_id = "btc_5m_up",
        .asset_index = kUpAssetIndex,
        .engine = mm::MarketMakingEngine(up_config)
    };
    TokenRuntime down{
        .direction = mm::ProductDirection::Down,
        .asset_id = "btc_5m_down",
        .asset_index = kDownAssetIndex,
        .engine = mm::MarketMakingEngine(down_config)
    };

    updown::UpDown5mQuoteControllerConfig controller_config;
    controller_config.base_max_canonical_lots = 60;
    controller_config.base_half_spread_tick = 15'000;
    controller_config.allow_directional_inventory = false;
    const updown::UpDown5mQuoteController controller(controller_config);
    const updown::UpDown5mFairProvider fair_provider;
    const auto policy = risk_policy();

    std::ofstream events(output_dir / "events.csv");
    if (!events) {
        throw std::runtime_error("failed to write events.csv");
    }
    write_event_header(&events);

    Metrics metrics;
    for (std::int64_t second = 0; second < kTotalSeconds; ++second) {
        ++metrics.seconds;
        const auto round = second / kRoundSeconds;
        const auto round_start_second = round * kRoundSeconds;
        const auto round_end_second = round_start_second + kRoundSeconds;
        const auto start_spot = spot_path(round_start_second);
        const auto spot = spot_path(second);
        const auto now_ns = static_cast<std::uint64_t>(second) * kNsPerSecond;
        const auto now_ms = second * 1'000;

        updown::UpDown5mMarketContext fair_context{
            .market_id = "btc_5m_synthetic_" + std::to_string(round),
            .up_token_id = up.asset_id,
            .down_token_id = down.asset_id,
            .asset_direction = mm::ProductDirection::Up,
            .round_start_ms = round_start_second * 1'000,
            .round_expiry_ms = round_end_second * 1'000,
            .now_ms = now_ms,
            .start_spot = start_spot,
            .current_spot = spot,
            .sigma_annualized = 0.55,
            .oracle_age_ms = 75.0 + static_cast<double>((second * 17) % 90),
            .polymarket_up_mid = 0.5,
            .polymarket_down_mid = 0.5,
            .settlement_source_ok = true,
            .book_ok = true
        };
        const auto fair = fair_provider.compute(fair_context);
        if (!fair.ok) {
            throw std::runtime_error("fair provider rejected synthetic context: " +
                                     fair.reject_reason);
        }

        const auto up_mid_tick =
            clamp_tick(fair.up_fair_tick + synthetic_lag_tick(second));
        const auto down_mid_tick =
            clamp_tick(mm::kPriceOneTick - up_mid_tick);
        fair_context.polymarket_up_mid =
            static_cast<double>(up_mid_tick) /
            static_cast<double>(mm::kPriceOneTick);
        fair_context.polymarket_down_mid =
            static_cast<double>(down_mid_tick) /
            static_cast<double>(mm::kPriceOneTick);

        const auto version = static_cast<std::uint64_t>(second + 1);
        const auto up_depth = make_depth(
            up_mid_tick,
            up.asset_index,
            version * 2,
            now_ns
        );
        const auto down_depth = make_depth(
            down_mid_tick,
            down.asset_index,
            version * 2 + 1,
            now_ns
        );

        fair_context.asset_direction = mm::ProductDirection::Up;
        const auto up_fair = fair_provider.compute(fair_context);
        run_token_step(
            &up,
            up,
            down,
            fair_context,
            up_depth,
            down_depth,
            up_mid_tick,
            up_fair,
            controller,
            policy,
            &metrics,
            &events,
            second,
            round,
            true,
            0,
            0
        );

        fair_context.asset_direction = mm::ProductDirection::Down;
        const auto down_fair = fair_provider.compute(fair_context);
        run_token_step(
            &down,
            up,
            down,
            fair_context,
            down_depth,
            up_depth,
            down_mid_tick,
            down_fair,
            controller,
            policy,
            &metrics,
            &events,
            second,
            round,
            true,
            0,
            0
        );

        metrics.max_abs_canonical_lots = std::max<std::int64_t>(
            metrics.max_abs_canonical_lots,
            std::llabs(canonical_position(up, down))
        );

        if ((second + 1) % kRoundSeconds == 0) {
            const auto final_spot = spot_path(second);
            settle_round(
                &up,
                &down,
                final_spot > start_spot,
                &metrics
            );
        }
    }

    write_summary(
        output_dir / "summary.json",
        metrics,
        up,
        down,
        "synthetic_30m_backtest",
        "No local BTC 5m historical replay fixture was found; this run uses a deterministic synthetic BTC path and synthetic maker fills."
    );

    std::cout << "synthetic 30m backtest complete\n";
    std::cout << "summary=" << (output_dir / "summary.json").string() << "\n";
    std::cout << "events=" << (output_dir / "events.csv").string() << "\n";
    std::cout << "quotes=" << metrics.quote_intents
              << " approved=" << metrics.risk_approved
              << " rejected=" << metrics.risk_rejected
              << " fills=" << metrics.fills
              << " pnl_tick=" << (up.cash_tick + down.cash_tick)
              << "\n";
    return 0;
}
