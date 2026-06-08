#pragma once

#include "engine/reward/public/RewardTypes.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace trading_engine::reward {

class RewardUniverseStore {
public:
    void update(RewardConfigSnapshot snapshot);
    void clear();

    [[nodiscard]] const RewardConfigSnapshot& latest_snapshot() const noexcept;
    [[nodiscard]] std::uint64_t refresh_ts_ns() const noexcept;
    [[nodiscard]] RewardSourceQuality source_quality() const noexcept;

    [[nodiscard]] const RewardMarketConfig* by_condition_id(
        const std::string& condition_id
    ) const noexcept;

    [[nodiscard]] const RewardMarketConfig* by_token_id(
        const std::string& token_id
    ) const noexcept;

private:
    RewardConfigSnapshot snapshot_;
    std::unordered_map<std::string, std::size_t> by_condition_id_;
    std::unordered_map<std::string, std::size_t> by_token_id_;

    void rebuild_indexes();
};

}  // namespace trading_engine::reward
