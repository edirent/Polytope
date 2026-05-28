#pragma once

#include "chain_confirm/DirectionLabel.h"

#include <cstdint>
#include <string>

namespace trading_engine::chain_confirm {

enum class FillMappingStatus : std::uint8_t {
    Mapped = 0,
    UnmappedFill,
    AmbiguousFill
};

struct ConfirmedFill {
    std::string fill_id;
    std::string order_hash;

    std::string market_id;
    std::string asset_id;

    std::int64_t price_tick{0};
    std::int64_t size_lots{0};

    ConfirmedDirection direction{ConfirmedDirection::Unknown};
    FillMappingStatus mapping_status{FillMappingStatus::UnmappedFill};

    std::uint64_t block_number{0};
    std::string tx_hash;
    std::uint32_t log_index{0};

    std::uint64_t chain_seen_monotonic_ns{0};

    bool removed{false};
};

[[nodiscard]] std::string fill_id(
    const std::string& tx_hash,
    std::uint32_t log_index
);

[[nodiscard]] std::string to_string(FillMappingStatus status);

}  // namespace trading_engine::chain_confirm
