#pragma once

#include <cstdint>

namespace trading_engine::strategy::market_making {

[[nodiscard]] inline bool quote_expired(
    std::uint64_t now_ns,
    std::uint64_t expires_at_ns
) noexcept {
    return expires_at_ns != 0 && now_ns >= expires_at_ns;
}

}  // namespace trading_engine::strategy::market_making
