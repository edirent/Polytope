#pragma once

#include "engine/execution/public/ExecutionTypes.h"

#include <cstdint>

namespace trading_engine::execution {

struct ExecutionConfig {
    ExecutionMode mode = ExecutionMode::Paper;

    bool execution_enabled = false;
    bool live_enabled = false;

    PaperExecutionMode paper_mode = PaperExecutionMode::PaperAtomic;
    bool allow_partial_fill_paper = false;

    std::uint32_t max_child_orders_per_plan = 16;
    std::uint32_t max_plans_per_second = 10;

    std::int64_t max_order_age_ns = 1'000'000'000;
    std::int64_t default_time_in_force_ns = 1'000'000'000;

    bool hot_path_trust_order_decision_hash = false;
    bool hot_path_trust_approval_hash = false;
    bool hot_path_skip_plan_validation = false;
    bool hot_path_skip_order_strings = false;
    bool hot_path_numeric_plan_id = false;
};

}  // namespace trading_engine::execution
