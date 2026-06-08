#include "engine/reward/store/RewardUniverseStore.h"

#include <utility>

namespace trading_engine::reward {

void RewardUniverseStore::update(RewardConfigSnapshot snapshot) {
    snapshot_ = std::move(snapshot);
    rebuild_indexes();
}

void RewardUniverseStore::clear() {
    snapshot_ = RewardConfigSnapshot{};
    by_condition_id_.clear();
    by_token_id_.clear();
}

const RewardConfigSnapshot& RewardUniverseStore::latest_snapshot()
    const noexcept {
    return snapshot_;
}

std::uint64_t RewardUniverseStore::refresh_ts_ns() const noexcept {
    return snapshot_.refresh_ts_ns;
}

RewardSourceQuality RewardUniverseStore::source_quality() const noexcept {
    return snapshot_.source_quality;
}

const RewardMarketConfig* RewardUniverseStore::by_condition_id(
    const std::string& condition_id
) const noexcept {
    const auto it = by_condition_id_.find(condition_id);
    if (it == by_condition_id_.end() || it->second >= snapshot_.markets.size()) {
        return nullptr;
    }
    return &snapshot_.markets[it->second];
}

const RewardMarketConfig* RewardUniverseStore::by_token_id(
    const std::string& token_id
) const noexcept {
    const auto it = by_token_id_.find(token_id);
    if (it == by_token_id_.end() || it->second >= snapshot_.markets.size()) {
        return nullptr;
    }
    return &snapshot_.markets[it->second];
}

void RewardUniverseStore::rebuild_indexes() {
    by_condition_id_.clear();
    by_token_id_.clear();
    for (std::size_t idx = 0; idx < snapshot_.markets.size(); ++idx) {
        const auto& market = snapshot_.markets[idx];
        if (!market.condition_id.empty()) {
            by_condition_id_.insert_or_assign(market.condition_id, idx);
        }
        for (const auto& token : market.tokens) {
            if (!token.token_id.empty()) {
                by_token_id_.insert_or_assign(token.token_id, idx);
            }
        }
    }
}

}  // namespace trading_engine::reward
