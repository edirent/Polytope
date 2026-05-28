#pragma once

#include "state/chain/ConfirmedTradeState.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace trading_engine::state {

struct ConfirmedTradeWindowTotals {
    std::int64_t buy_lots{0};
    std::int64_t sell_lots{0};
};

class ConfirmedTradeWindow {
public:
    void add(
        std::string fill_id,
        std::uint64_t chain_seen_ns,
        AggressorSide side,
        std::int64_t size_lots
    ) {
        entries_.push_back({
            std::move(fill_id),
            chain_seen_ns,
            side,
            size_lots,
            false
        });
    }

    [[nodiscard]] bool mark_removed(const std::string& fill_id) {
        bool removed = false;
        for (auto& entry : entries_) {
            if (entry.fill_id == fill_id && !entry.removed) {
                entry.removed = true;
                removed = true;
            }
        }
        return removed;
    }

    [[nodiscard]] ConfirmedTradeWindowTotals totals(
        std::uint64_t now_ns,
        std::uint64_t window_ns
    ) const {
        ConfirmedTradeWindowTotals out;
        for (const auto& entry : entries_) {
            if (entry.removed || entry.chain_seen_ns > now_ns) {
                continue;
            }

            if (now_ns - entry.chain_seen_ns > window_ns) {
                continue;
            }

            if (entry.side == AggressorSide::Buy) {
                out.buy_lots += entry.size_lots;
            } else if (entry.side == AggressorSide::Sell) {
                out.sell_lots += entry.size_lots;
            }
        }
        return out;
    }

private:
    struct Entry {
        std::string fill_id;
        std::uint64_t chain_seen_ns{0};
        AggressorSide side{AggressorSide::Unknown};
        std::int64_t size_lots{0};
        bool removed{false};
    };

    std::vector<Entry> entries_;
};

}  // namespace trading_engine::state
