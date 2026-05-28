#pragma once

#include <cstdint>
#include <string>

namespace trading_engine::state {

constexpr std::uint32_t kMaxSnapshotDepth = 64;
constexpr std::int64_t kDefaultPriceScale = 1'000'000;

struct PriceLevel {
    std::int64_t price_tick{0};
    double price{0.0};
    double size{0.0};
};

struct BestBidAsk {
    PriceLevel bid;
    PriceLevel ask;
};

[[nodiscard]] inline std::string to_entity_id(std::uint64_t entity_id) {
    return std::to_string(entity_id);
}

}  // namespace trading_engine::state
