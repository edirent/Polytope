#pragma once

#include <cstdint>
#include <string>

namespace trading_engine::state {

enum class BookQuality : std::uint8_t {
    Unknown = 0,
    Good,
    Stale,
    Recovering,
    Crossed,
    ChainMismatch,
    ChainLagging,
    Closed,
    Resolved
};

struct BookQualityState {
    BookQuality quality{BookQuality::Unknown};

    bool ws_live{false};
    bool chain_live{false};

    std::uint64_t last_ws_recv_ns{0};
    std::uint64_t last_chain_seen_ns{0};

    std::uint32_t ws_decode_errors_recent{0};
    std::uint32_t state_errors_recent{0};
    std::uint32_t chain_decode_errors_recent{0};

    std::uint32_t chain_ws_mismatch_count_recent{0};

    bool usable_for_depth{false};
    bool usable_for_signal{false};
};

[[nodiscard]] std::string to_string(BookQuality quality);

}  // namespace trading_engine::state
