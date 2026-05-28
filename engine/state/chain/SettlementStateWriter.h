#pragma once

#include "state/chain/SettlementState.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace trading_engine::state {

enum class SettlementApplyCode : std::uint8_t {
    Applied = 0,
    MissingMarketId
};

struct SettlementApplyResult {
    SettlementApplyCode code{SettlementApplyCode::Applied};
    std::string market_id;
    SettlementStatus status{SettlementStatus::Unknown};
    bool state_changed{false};
    std::string message;

    [[nodiscard]] bool ok() const noexcept {
        return code == SettlementApplyCode::Applied;
    }
};

class SettlementStateWriter {
public:
    SettlementApplyResult mark_open(
        const std::string& market_id,
        std::uint64_t block_number
    );

    SettlementApplyResult mark_closed(
        const std::string& market_id,
        std::uint64_t block_number
    );

    SettlementApplyResult mark_resolved(
        const std::string& market_id,
        const std::string& winning_asset_id,
        std::uint64_t block_number
    );

    [[nodiscard]] const SettlementState* get(
        const std::string& market_id
    ) const noexcept;

    [[nodiscard]] bool contains(const std::string& market_id) const noexcept;

private:
    SettlementApplyResult apply_status(
        const std::string& market_id,
        SettlementStatus status,
        std::string winning_asset_id,
        std::uint64_t block_number
    );

private:
    std::unordered_map<std::string, SettlementState> states_;
};

[[nodiscard]] const char* to_string(SettlementApplyCode code) noexcept;

}  // namespace trading_engine::state
