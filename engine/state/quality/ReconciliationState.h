#pragma once

#include <cstdint>

namespace trading_engine::state {

struct ReconciliationState {
    std::uint64_t version{0};
    std::uint64_t last_update_ns{0};

    std::uint32_t chain_ws_match_count_recent{0};
    std::uint32_t chain_ws_mismatch_count_recent{0};
    std::uint32_t ambiguous_count_recent{0};
    std::uint32_t unmatched_count_recent{0};

    void record_match(std::uint64_t now_ns) noexcept;
    void record_mismatch(std::uint64_t now_ns) noexcept;
    void record_ambiguous(std::uint64_t now_ns) noexcept;
    void record_unmatched(std::uint64_t now_ns) noexcept;
};

}  // namespace trading_engine::state
