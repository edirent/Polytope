#pragma once

#include <cstdint>

namespace trading_engine::chain_confirm {

struct ChainConfirmMetrics {
    std::uint64_t ws_logs_received{0};
    std::uint64_t http_backfill_requests{0};
    std::uint64_t order_filled_decoded{0};
    std::uint64_t decode_errors{0};
    std::uint64_t confirmed_fills{0};
    std::uint64_t removed_logs{0};
    std::uint64_t unmatched_hints{0};
    std::uint64_t unmatched_fills{0};
    std::uint64_t ambiguous_matches{0};
};

}  // namespace trading_engine::chain_confirm
