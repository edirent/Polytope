#pragma once

#include "chain_confirm/PendingTradeHint.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace trading_engine::chain_confirm {

class PendingTradeHintRing {
public:
    explicit PendingTradeHintRing(std::size_t capacity = 4096);

    void push(const PendingTradeHint& hint);

    [[nodiscard]] std::vector<PendingTradeHint*> candidates(
        const std::string& market_id,
        const std::string& asset_id,
        std::int64_t price_tick,
        std::int64_t size_lots,
        std::uint64_t target_monotonic_ns,
        std::uint64_t window_ns
    );

    [[nodiscard]] std::vector<PendingTradeHint> expire(
        std::uint64_t now_monotonic_ns,
        std::uint64_t max_age_ns
    );

    [[nodiscard]] std::size_t size() const noexcept;
    void clear();

private:
    std::size_t capacity_{4096};
    std::deque<PendingTradeHint> hints_;
};

}  // namespace trading_engine::chain_confirm
