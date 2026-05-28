#pragma once

#include "chain_confirm/ConfirmedFill.h"

#include <cstdint>
#include <string>

namespace trading_engine::chain_confirm {

enum class FillClassification : std::uint8_t {
    Unknown = 0,
    ChainConfirmed,
    ChainRemoved,
    UnmappedFill,
    AmbiguousFill
};

struct ClassifiedFillRecord {
    std::string fill_id;
    std::string order_hash;

    std::string market_id;
    std::string asset_id;

    std::int64_t price_tick{0};
    std::int64_t size_lots{0};

    ConfirmedDirection direction{ConfirmedDirection::Unknown};
    FillMappingStatus mapping_status{FillMappingStatus::UnmappedFill};
    FillClassification classification{FillClassification::Unknown};

    std::uint64_t block_number{0};
    std::string tx_hash;
    std::uint32_t log_index{0};

    std::uint64_t chain_seen_monotonic_ns{0};
    std::uint64_t source_sequence{0};

    bool removed{false};
};

[[nodiscard]] ClassifiedFillRecord classify_confirmed_fill(
    const ConfirmedFill& fill,
    std::uint64_t source_sequence = 0
);

[[nodiscard]] std::string to_string(FillClassification classification);

}  // namespace trading_engine::chain_confirm
