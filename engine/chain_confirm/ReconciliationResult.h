#pragma once

#include "chain_confirm/DirectionLabel.h"

#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::chain_confirm {

enum class ReconciliationStatus : std::uint8_t {
    ConfirmedOneToOne = 0,
    ConfirmedOneToMany,
    ConfirmedManyToOne,
    Ambiguous,
    UnmatchedHint,
    UnmatchedFill,
    ExpiredHint,
    RemovedByReorg
};

struct ReconciliationResult {
    ReconciliationStatus status{ReconciliationStatus::UnmatchedHint};

    std::string fill_id;
    std::vector<std::uint64_t> hint_ids;

    std::string market_id;
    std::string asset_id;
    std::int64_t price_tick{0};
    std::int64_t size_lots{0};

    HintDirection hint_direction{HintDirection::Unknown};
    ConfirmedDirection confirmed_direction{ConfirmedDirection::Unknown};

    std::uint32_t candidate_count{0};
    bool finalized{false};
    std::string reason;
};

[[nodiscard]] std::string to_string(ReconciliationStatus status);

}  // namespace trading_engine::chain_confirm
