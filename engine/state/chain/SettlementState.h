#pragma once

#include <cstdint>
#include <string>

namespace trading_engine::state {

enum class SettlementStatus : std::uint8_t {
    Unknown = 0,
    Open,
    Closed,
    Resolved
};

struct SettlementState {
    SettlementStatus status{SettlementStatus::Open};

    bool resolved{false};
    std::string winning_asset_id;

    std::uint64_t last_update_block{0};
    std::uint64_t version{0};
};

[[nodiscard]] const char* to_string(SettlementStatus status) noexcept;

}  // namespace trading_engine::state
