#include "chain_confirm/PendingTradeHintRing.h"

#include <algorithm>

namespace trading_engine::chain_confirm {

PendingTradeHintRing::PendingTradeHintRing(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

void PendingTradeHintRing::push(const PendingTradeHint& hint) {
    if (hints_.size() >= capacity_) {
        hints_.pop_front();
    }

    hints_.push_back(hint);
}

std::vector<PendingTradeHint*> PendingTradeHintRing::candidates(
    const std::string& market_id,
    const std::string& asset_id,
    std::int64_t price_tick,
    std::int64_t size_lots,
    std::uint64_t target_monotonic_ns,
    std::uint64_t window_ns
) {
    std::vector<PendingTradeHint*> matches;

    for (auto& hint : hints_) {
        if (hint.finalized) {
            continue;
        }

        if (hint.market_id != market_id ||
            hint.asset_id != asset_id ||
            hint.price_tick != price_tick ||
            hint.size_lots != size_lots) {
            continue;
        }

        const auto lhs = hint.ws_recv_monotonic_ns;
        const auto rhs = target_monotonic_ns;
        const auto age = lhs >= rhs ? lhs - rhs : rhs - lhs;
        if (age <= window_ns) {
            matches.push_back(&hint);
        }
    }

    std::sort(
        matches.begin(),
        matches.end(),
        [target_monotonic_ns](const auto* lhs, const auto* rhs) {
            const auto lhs_age =
                lhs->ws_recv_monotonic_ns >= target_monotonic_ns
                    ? lhs->ws_recv_monotonic_ns - target_monotonic_ns
                    : target_monotonic_ns - lhs->ws_recv_monotonic_ns;
            const auto rhs_age =
                rhs->ws_recv_monotonic_ns >= target_monotonic_ns
                    ? rhs->ws_recv_monotonic_ns - target_monotonic_ns
                    : target_monotonic_ns - rhs->ws_recv_monotonic_ns;
            return lhs_age < rhs_age;
        }
    );

    return matches;
}

std::vector<PendingTradeHint> PendingTradeHintRing::expire(
    std::uint64_t now_monotonic_ns,
    std::uint64_t max_age_ns
) {
    std::vector<PendingTradeHint> expired;

    auto it = hints_.begin();
    while (it != hints_.end()) {
        const auto age = now_monotonic_ns >= it->ws_recv_monotonic_ns
            ? now_monotonic_ns - it->ws_recv_monotonic_ns
            : 0;

        if (!it->finalized && age > max_age_ns) {
            expired.push_back(*it);
            it = hints_.erase(it);
            continue;
        }

        ++it;
    }

    return expired;
}

std::size_t PendingTradeHintRing::size() const noexcept {
    return hints_.size();
}

void PendingTradeHintRing::clear() {
    hints_.clear();
}

}  // namespace trading_engine::chain_confirm
