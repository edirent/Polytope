#pragma once

#include "engine/execution/public/ExecutionTypes.h"
#include "engine/paper/portfolio/ExposureView.h"
#include "engine/paper/portfolio/Position.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace trading_engine::paper {

using PaperPosition = Position;

enum class PositionLedgerApplyStatus : std::uint8_t {
    Applied,
    InvalidFill,
    UnsupportedSell
};

struct PositionLedgerApplyResult {
    bool applied = false;
    PositionLedgerApplyStatus status = PositionLedgerApplyStatus::InvalidFill;
};

struct PositionFill {
    std::string asset_id;
    std::uint32_t asset_index = 0;
    trading_engine::execution::OrderSide side =
        trading_engine::execution::OrderSide::Buy;
    std::int64_t qty_lots = 0;
    std::int64_t price_tick = 0;
};

class PositionLedger {
public:
    [[nodiscard]] PositionLedgerApplyResult apply_fill(const PositionFill& fill);

    void apply_buy(
        const std::string& asset_id,
        std::int64_t lots,
        std::int64_t cost_tick
    );
    void apply_buy(
        const std::string& asset_id,
        std::uint32_t asset_index,
        std::int64_t lots,
        std::int64_t cost_tick
    );

    void mark_mid(const std::string& asset_id, std::int64_t mid_tick);
    void mark_liquidation(
        const std::string& asset_id,
        std::int64_t liquidation_tick
    );

    [[nodiscard]] const PaperPosition* find(const std::string& asset_id) const;
    [[nodiscard]] std::int64_t lots(const std::string& asset_id) const;
    [[nodiscard]] std::int64_t cost_basis_tick(const std::string& asset_id) const;
    [[nodiscard]] std::int64_t avg_cost_tick(const std::string& asset_id) const;
    [[nodiscard]] std::size_t asset_count() const noexcept;
    [[nodiscard]] ExposureView exposure() const;
    [[nodiscard]] const std::map<std::string, PaperPosition>& positions() const noexcept;

private:
    std::map<std::string, PaperPosition> positions_;
};

}  // namespace trading_engine::paper
