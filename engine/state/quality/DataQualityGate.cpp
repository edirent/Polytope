#include "state/quality/DataQualityGate.h"

namespace trading_engine::state {

namespace {

[[nodiscard]] bool age_exceeds(
    std::uint64_t now_ns,
    std::uint64_t last_seen_ns,
    std::uint64_t timeout_ns
) noexcept {
    if (now_ns == 0 || last_seen_ns == 0) {
        return false;
    }

    if (now_ns < last_seen_ns) {
        return false;
    }

    return now_ns - last_seen_ns > timeout_ns;
}

[[nodiscard]] bool crossed_book(const EntityState& entity) noexcept {
    if (entity.book.crossed) {
        return true;
    }

    if (!entity.book.best_bid.has_value() ||
        !entity.book.best_ask.has_value()) {
        return false;
    }

    return entity.book.best_bid.value() > entity.book.best_ask.value();
}

void set_usability(BookQualityState& state) noexcept {
    switch (state.quality) {
        case BookQuality::Good:
            state.usable_for_depth = true;
            state.usable_for_signal = true;
            break;

        case BookQuality::ChainLagging:
        case BookQuality::ChainMismatch:
            state.usable_for_depth = true;
            state.usable_for_signal = false;
            break;

        case BookQuality::Unknown:
        case BookQuality::Stale:
        case BookQuality::Recovering:
        case BookQuality::Crossed:
        case BookQuality::Closed:
        case BookQuality::Resolved:
        default:
            state.usable_for_depth = false;
            state.usable_for_signal = false;
            break;
    }
}

}  // namespace

DataQualityGate::DataQualityGate(DataQualityGateConfig config) noexcept
    : config_(config) {}

BookQualityState DataQualityGate::evaluate(
    const DataQualityInput& input
) const noexcept {
    BookQualityState out;
    out.ws_decode_errors_recent = input.ws_decode_errors_recent;
    out.state_errors_recent = input.state_errors_recent;
    out.chain_decode_errors_recent = input.chain_decode_errors_recent;
    out.chain_ws_mismatch_count_recent =
        input.reconciliation.chain_ws_mismatch_count_recent;

    if (input.entity) {
        out.last_ws_recv_ns = input.entity->last_update_monotonic_ns;
    }

    if (input.confirmed_trade_state) {
        out.last_chain_seen_ns =
            input.confirmed_trade_state->last_chain_seen_ns;
    }

    out.ws_live = ws_live(input);
    out.chain_live = chain_live(input);

    if (!input.entity ||
        !input.entity->initialized ||
        input.entity->recovering ||
        input.entity->status == EntityStatus::Recovering ||
        input.state_errors_recent > 0) {
        out.quality = BookQuality::Recovering;
        set_usability(out);
        return out;
    }

    if (input.entity->closed ||
        input.entity->status == EntityStatus::Closed) {
        out.quality = BookQuality::Closed;
        set_usability(out);
        return out;
    }

    if (input.entity->book.resolved) {
        out.quality = BookQuality::Resolved;
        set_usability(out);
        return out;
    }

    if (crossed_book(*input.entity)) {
        out.quality = BookQuality::Crossed;
        set_usability(out);
        return out;
    }

    if (!out.ws_live) {
        out.quality = BookQuality::Stale;
        set_usability(out);
        return out;
    }

    if (!out.chain_live || input.chain_decode_errors_recent > 0) {
        out.quality = BookQuality::ChainLagging;
        set_usability(out);
        return out;
    }

    if (input.reconciliation.chain_ws_mismatch_count_recent > 0) {
        out.quality = BookQuality::ChainMismatch;
        set_usability(out);
        return out;
    }

    out.quality = BookQuality::Good;
    set_usability(out);
    return out;
}

bool DataQualityGate::ws_live(
    const DataQualityInput& input
) const noexcept {
    if (!input.entity || input.entity->last_update_monotonic_ns == 0) {
        return false;
    }

    return !age_exceeds(
        input.now_ns,
        input.entity->last_update_monotonic_ns,
        config_.ws_stale_timeout_ns
    );
}

bool DataQualityGate::chain_live(
    const DataQualityInput& input
) const noexcept {
    if (!input.confirmed_trade_state ||
        input.confirmed_trade_state->last_chain_seen_ns == 0) {
        return false;
    }

    return !age_exceeds(
        input.now_ns,
        input.confirmed_trade_state->last_chain_seen_ns,
        config_.chain_lag_timeout_ns
    );
}

}  // namespace trading_engine::state
