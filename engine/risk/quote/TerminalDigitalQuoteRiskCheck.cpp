#include "engine/risk/quote/TerminalDigitalQuoteRiskCheck.h"

#include <cstdlib>

namespace trading_engine::risk {

namespace mm = strategy::market_making;

namespace {

[[nodiscard]] bool has_opening_side(
    const mm::QuoteIntent& quote
) noexcept {
    return (quote.has_bid && !quote.bid.risk_reducing) ||
           (quote.has_ask && !quote.ask.risk_reducing);
}

[[nodiscard]] bool active_opening_side_deviates_from_fair(
    const mm::QuoteIntent& quote,
    std::int64_t product_fair_tick,
    std::int64_t max_deviation_tick
) noexcept {
    if (product_fair_tick <= 0 || max_deviation_tick <= 0) {
        return false;
    }
    if (quote.has_bid && !quote.bid.risk_reducing &&
        std::llabs(quote.bid.price_tick - product_fair_tick) >
            max_deviation_tick) {
        return true;
    }
    if (quote.has_ask && !quote.ask.risk_reducing &&
        std::llabs(quote.ask.price_tick - product_fair_tick) >
            max_deviation_tick) {
        return true;
    }
    return false;
}

[[nodiscard]] TerminalDigitalQuoteRiskCheckResult reject(
    QuoteRiskDecisionType decision,
    const char* reason
) noexcept {
    return TerminalDigitalQuoteRiskCheckResult{
        .rejected = true,
        .decision = decision,
        .reason = reason
    };
}

}  // namespace

TerminalDigitalQuoteRiskCheckResult TerminalDigitalQuoteRiskCheck::evaluate(
    const QuoteRiskInput& input
) const noexcept {
    if (!input.quote || !input.policy ||
        !input.policy->terminal_digital_5m_guard_enabled ||
        input.quote->product_kind != mm::ProductKind::TerminalDigital5m) {
        return {};
    }

    const auto& quote = *input.quote;
    const auto& policy = *input.policy;
    if (!quote.product_settlement_source_ok) {
        return reject(
            QuoteRiskDecisionType::RejectInvalidQuote,
            "terminal digital settlement source unavailable"
        );
    }
    if (policy.terminal_digital_max_oracle_age_ms > 0 &&
        input.spot_age_ms >
            static_cast<std::int64_t>(
                policy.terminal_digital_max_oracle_age_ms
            )) {
        return reject(
            QuoteRiskDecisionType::RejectStaleSpot,
            "terminal digital stale oracle"
        );
    }

    const auto opening = has_opening_side(quote);
    if (!opening) {
        return {};
    }

    if (!quote.product_opening_allowed) {
        return reject(
            QuoteRiskDecisionType::RejectInvalidQuote,
            "terminal digital opening gate disabled"
        );
    }
    if (policy.terminal_digital_min_quote_tau_ms > 0 &&
        quote.product_tau_ms < policy.terminal_digital_min_quote_tau_ms) {
        return reject(
            QuoteRiskDecisionType::RejectExpiredQuote,
            "terminal digital tau below opening minimum"
        );
    }
    if (policy.terminal_digital_max_opening_z_bps > 0 &&
        quote.product_z_score_bps >
            policy.terminal_digital_max_opening_z_bps) {
        return reject(
            QuoteRiskDecisionType::RejectFairValueDeviation,
            "terminal digital z score above opening gate"
        );
    }
    if (policy.terminal_digital_losing_side_z_cutoff_bps > 0 &&
        quote.product_z_score_bps >
            policy.terminal_digital_losing_side_z_cutoff_bps &&
        quote.product_asset_direction != mm::ProductDirection::Unknown &&
        quote.product_winning_direction != mm::ProductDirection::Unknown &&
        quote.product_asset_direction != quote.product_winning_direction) {
        return reject(
            QuoteRiskDecisionType::RejectFairValueDeviation,
            "terminal digital losing side above z cutoff"
        );
    }
    if (quote.product_dynamic_inventory_cap_lots >= 0 &&
        std::llabs(input.projected_canonical_yes_position_lots) >
            quote.product_dynamic_inventory_cap_lots) {
        return reject(
            QuoteRiskDecisionType::RejectCanonicalExposureLimit,
            "terminal digital dynamic inventory cap exceeded"
        );
    }
    if (active_opening_side_deviates_from_fair(
            quote,
            quote.product_external_fair_tick,
            policy.terminal_digital_max_fair_deviation_tick
        )) {
        return reject(
            QuoteRiskDecisionType::RejectFairValueDeviation,
            "terminal digital quote deviates from external fair"
        );
    }
    return {};
}

}  // namespace trading_engine::risk
