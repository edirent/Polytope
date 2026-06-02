#pragma once

#include "engine/strategy/market_making/public/QuoteIntent.h"

#include <cstdint>

namespace trading_engine::risk {

using QuoteLeg = strategy::market_making::QuoteLeg;

struct ApprovedQuote {
    std::uint64_t approved_quote_id = 0;
    std::uint64_t quote_intent_id = 0;
    std::uint64_t quote_group_id = 0;

    bool has_bid = false;
    bool has_ask = false;

    QuoteLeg bid;
    QuoteLeg ask;

    std::uint64_t approved_ts_ns = 0;
    std::uint64_t expires_at_ns = 0;

    std::uint64_t idempotency_hash = 0;
    std::uint64_t policy_hash = 0;
    std::uint64_t snapshot_version_hash = 0;
};

[[nodiscard]] std::uint64_t compute_approved_quote_hash(
    const ApprovedQuote& quote
) noexcept;

}  // namespace trading_engine::risk
