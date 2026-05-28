#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace trading_engine::oracle {

enum class Side : std::uint8_t {
    Buy,
    Sell
};

struct BundleLeg {
    std::string market_id;
    std::string asset_id;

    Side side = Side::Buy;

    std::int64_t quantity_lots = 0;
    std::int64_t max_price_tick = 0;
};

inline constexpr std::uint16_t kMaxBundleLegs = 16;

struct CandidateBundle {
    std::uint64_t bundle_id = 0;

    std::uint64_t required_true_mask = 0;
    std::uint64_t required_false_mask = 0;
    std::uint64_t invalid_mask = 0;

    std::int64_t guaranteed_payout_tick = 0;
    std::uint16_t leg_count = 0;

    std::array<BundleLeg, kMaxBundleLegs> legs{};

    std::int64_t min_edge_tick = 0;
};

[[nodiscard]] const char* side_to_string(Side side) noexcept;
[[nodiscard]] bool side_from_string(
    const std::string& value,
    Side* out
) noexcept;

}  // namespace trading_engine::oracle
