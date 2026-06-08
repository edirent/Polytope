#pragma once

#include <string>

namespace trading_engine::reward {

struct PrivateRewardAccountResult {
    bool ok = false;
    double reconciled_reward_tick = 0.0;
    std::string error;
};

class PrivateRewardAccountClient {
public:
    [[nodiscard]] PrivateRewardAccountResult fetch_user_reward_markets() const;
};

}  // namespace trading_engine::reward
