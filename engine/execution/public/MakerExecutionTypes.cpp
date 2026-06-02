#include "engine/execution/public/MakerExecutionTypes.h"

namespace trading_engine::execution {

namespace {

[[nodiscard]] std::uint64_t mix(
    std::uint64_t hash,
    std::uint64_t value
) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 0xffU;
        hash *= 1099511628211ULL;
    }
    return hash;
}

}  // namespace

std::uint64_t compute_paper_maker_quote_id(
    const risk::ApprovedQuote& quote
) noexcept {
    auto hash = 14695981039346656037ULL;
    hash = mix(hash, quote.approved_quote_id);
    hash = mix(hash, quote.quote_intent_id);
    hash = mix(hash, quote.quote_group_id);
    hash = mix(hash, quote.idempotency_hash);
    hash = mix(hash, quote.policy_hash);
    hash = mix(hash, quote.snapshot_version_hash);
    return hash == 0 ? 1 : hash;
}

std::uint64_t compute_maker_execution_report_hash(
    const MakerExecutionReport& report
) noexcept {
    auto hash = 14695981039346656037ULL;
    hash = mix(hash, report.quote_id);
    hash = mix(hash, report.approved_quote_id);
    hash = mix(hash, report.quote_group_id);
    hash = mix(hash, report.asset_index);
    hash = mix(hash, static_cast<std::uint8_t>(report.side));
    hash = mix(hash, static_cast<std::uint8_t>(report.status));
    hash = mix(hash, static_cast<std::uint8_t>(report.liquidity_role));
    hash = mix(hash, static_cast<std::uint64_t>(report.filled_qty_lots));
    hash = mix(hash, static_cast<std::uint64_t>(report.avg_fill_price_tick));
    hash = mix(hash, static_cast<std::uint64_t>(report.remaining_qty_lots));
    hash = mix(hash, report.exchange_ts_ns);
    hash = mix(hash, report.recv_ts_ns);
    return hash == 0 ? 1 : hash;
}

}  // namespace trading_engine::execution
