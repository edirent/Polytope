#include "state/quality/ReconciliationState.h"

namespace trading_engine::state {

void ReconciliationState::record_match(std::uint64_t now_ns) noexcept {
    ++version;
    last_update_ns = now_ns;
    ++chain_ws_match_count_recent;
}

void ReconciliationState::record_mismatch(std::uint64_t now_ns) noexcept {
    ++version;
    last_update_ns = now_ns;
    ++chain_ws_mismatch_count_recent;
}

void ReconciliationState::record_ambiguous(std::uint64_t now_ns) noexcept {
    ++version;
    last_update_ns = now_ns;
    ++ambiguous_count_recent;
}

void ReconciliationState::record_unmatched(std::uint64_t now_ns) noexcept {
    ++version;
    last_update_ns = now_ns;
    ++unmatched_count_recent;
}

}  // namespace trading_engine::state
