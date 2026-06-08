#pragma once

#include "engine/reward/public/RewardTypes.h"

#include <string>

namespace trading_engine::reward {

struct RewardClientResult {
    RewardConfigSnapshot snapshot;
    bool ok = false;
    std::string error;
};

class PublicRewardConfigClient {
public:
    [[nodiscard]] RewardClientResult fetch_current_rewards(
        bool sponsored
    ) const;

    [[nodiscard]] RewardClientResult fetch_market_rewards(
        const std::string& condition_id
    ) const;
};

}  // namespace trading_engine::reward
