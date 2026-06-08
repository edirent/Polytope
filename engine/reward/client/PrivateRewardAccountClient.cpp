#include "engine/reward/client/PrivateRewardAccountClient.h"

namespace trading_engine::reward {

PrivateRewardAccountResult
PrivateRewardAccountClient::fetch_user_reward_markets() const {
    PrivateRewardAccountResult result;
    result.ok = false;
    result.error = "private reward account client requires CLOB L2 auth wiring";
    return result;
}

}  // namespace trading_engine::reward
