#pragma once

#include <cstdint>

namespace trading_engine::order_decision {

struct OrderDecisionWorkflowSummary {
    std::uint64_t intents_loaded = 0;
    std::uint64_t decisions_created = 0;
    std::uint64_t rejected_low_edge = 0;
    std::uint64_t rejected_insufficient_depth = 0;
    std::int64_t avg_chosen_bundle_qty = 0;
    std::int64_t total_expected_edge = 0;
    std::uint64_t decision_hash = 0;
    bool determinism_passed = false;
};

}  // namespace trading_engine::order_decision
