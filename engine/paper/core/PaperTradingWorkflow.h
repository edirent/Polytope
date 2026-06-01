#pragma once

#include "engine/paper/core/PaperTradingEngine.h"

#include <cstddef>
#include <cstdint>

namespace trading_engine::paper {

class PaperTradingWorkflow {
public:
    explicit PaperTradingWorkflow(
        std::int64_t starting_cash_tick = 0,
        std::size_t dashboard_capacity = 256
    );

    [[nodiscard]] PaperTradingEngine& engine() noexcept;
    [[nodiscard]] const PaperTradingEngine& engine() const noexcept;

    [[nodiscard]] DashboardSnapshot latest_dashboard_snapshot() const;

private:
    PaperTradingEngine engine_;
};

}  // namespace trading_engine::paper
