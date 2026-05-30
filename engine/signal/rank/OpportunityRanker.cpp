#include "engine/signal/rank/OpportunityRanker.h"

#include <algorithm>

namespace trading_engine::signal {

namespace {

[[nodiscard]] int status_priority(IntentStatus status) noexcept {
    return status == IntentStatus::PaperOpportunity ? 0 : 1;
}

[[nodiscard]] bool intent_less(
    const OpportunityIntent& left,
    const OpportunityIntent& right
) noexcept {
    const auto left_status = status_priority(left.status);
    const auto right_status = status_priority(right.status);
    if (left_status != right_status) {
        return left_status < right_status;
    }
    if (left.total_edge_tick != right.total_edge_tick) {
        return left.total_edge_tick > right.total_edge_tick;
    }
    if (left.edge_bps != right.edge_bps) {
        return left.edge_bps > right.edge_bps;
    }
    if (left.bundle_qty != right.bundle_qty) {
        return left.bundle_qty > right.bundle_qty;
    }
    if (left.bundle_id != right.bundle_id) {
        return left.bundle_id < right.bundle_id;
    }
    return left.intent_id < right.intent_id;
}

}  // namespace

void OpportunityRanker::rank(
    std::vector<OpportunityIntent>* intents
) const {
    if (!intents) {
        return;
    }
    std::sort(intents->begin(), intents->end(), intent_less);
}

}  // namespace trading_engine::signal
