#include "engine/paper/core/PaperTradingWorkflow.h"

namespace trading_engine::paper {

PaperTradingWorkflow::PaperTradingWorkflow(
    std::int64_t starting_cash_tick,
    std::size_t dashboard_capacity
)
    : engine_(starting_cash_tick, dashboard_capacity) {}

PaperTradingEngine& PaperTradingWorkflow::engine() noexcept {
    return engine_;
}

const PaperTradingEngine& PaperTradingWorkflow::engine() const noexcept {
    return engine_;
}

DashboardSnapshot PaperTradingWorkflow::latest_dashboard_snapshot() const {
    return engine_.latest_dashboard_snapshot();
}

}  // namespace trading_engine::paper
