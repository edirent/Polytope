#include "engine/reward/client/PublicRewardConfigClient.h"

namespace trading_engine::reward {

RewardClientResult PublicRewardConfigClient::fetch_current_rewards(
    bool sponsored
) const {
    RewardClientResult result;
    result.snapshot.source = sponsored
        ? "polymarket_current_rewards_sponsored_stub"
        : "polymarket_current_rewards_stub";
    result.snapshot.source_quality = RewardSourceQuality::Unavailable;
    result.ok = false;
    result.error = "public reward HTTP client not wired";
    return result;
}

RewardClientResult PublicRewardConfigClient::fetch_market_rewards(
    const std::string& condition_id
) const {
    RewardClientResult result;
    result.snapshot.source = "polymarket_market_rewards_stub";
    result.snapshot.source_quality = RewardSourceQuality::Unavailable;
    result.ok = false;
    result.error = condition_id.empty()
        ? "missing condition_id"
        : "public reward HTTP client not wired";
    return result;
}

}  // namespace trading_engine::reward
