#pragma once

#include <string>
#include <unordered_set>

namespace trading_engine::state {

struct StateUniverse {
    std::unordered_set<std::string> active_asset_ids;
    std::unordered_set<std::string> active_market_ids;

    bool allow_heartbeats = true;
    bool allow_market_lifecycle_for_active_markets = true;
};

}  // namespace trading_engine::state
