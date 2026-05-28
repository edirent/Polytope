#pragma once

#include <cstdint>
#include <string>

namespace trading_engine::state {

class ShardRouter {
public:
    static constexpr std::uint32_t kNumShards = 1;

    [[nodiscard]] std::uint32_t shard_for_asset(
        const std::string& asset_id
    ) const noexcept {
        (void)asset_id;
        return 0;
    }
};

}  // namespace trading_engine::state
