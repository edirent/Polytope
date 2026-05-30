#pragma once

#include <cstdint>
#include <string>

namespace trading_engine::signal {

struct IntentIdentityInput {
    std::uint64_t bundle_id = 0;
    std::uint64_t bundle_hash = 0;
    std::uint64_t snapshot_version_hash = 0;
    std::int64_t bundle_qty = 0;
    std::int64_t unit_edge_tick = 0;
};

[[nodiscard]] std::uint64_t make_intent_id(
    const IntentIdentityInput& input
) noexcept;

[[nodiscard]] std::string make_idempotency_key(
    const IntentIdentityInput& input
);

[[nodiscard]] std::string hex_u64(std::uint64_t value);

}  // namespace trading_engine::signal
