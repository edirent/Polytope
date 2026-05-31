#pragma once

#include "engine/execution/public/ChildOrder.h"
#include "engine/execution/public/ExecutionReport.h"
#include "engine/execution/public/OrderPlan.h"
#include "engine/execution/state/FillTracker.h"

#include <array>
#include <cstdint>

namespace trading_engine::execution {

struct FillTrackerScratch {
    PlanFillState plan_state;

    void reset() noexcept {
        plan_state = PlanFillState{};
    }
};

struct ExecutionScratch {
    std::array<ChildOrder, kMaxChildOrdersPerPlan> child_orders{};
    std::uint16_t child_order_count = 0;

    std::array<ExecutionReport, kMaxChildOrdersPerPlan> child_reports{};
    std::uint16_t child_report_count = 0;

    FillTrackerScratch fill_tracker;

    void reset() noexcept {
        child_order_count = 0;
        child_report_count = 0;
        fill_tracker.reset();
    }

    [[nodiscard]] bool push_child_order(const ChildOrder& order) {
        if (child_order_count >= child_orders.size()) {
            return false;
        }
        child_orders[child_order_count++] = order;
        return true;
    }

    [[nodiscard]] bool push_child_report(const ExecutionReport& report) {
        if (child_report_count >= child_reports.size()) {
            return false;
        }
        child_reports[child_report_count++] = report;
        return true;
    }
};

}  // namespace trading_engine::execution
