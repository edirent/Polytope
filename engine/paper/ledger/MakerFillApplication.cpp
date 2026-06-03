#include "engine/paper/ledger/MakerFillApplication.h"

#include <utility>

namespace trading_engine::paper {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void mix_u64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        *hash ^= (value >> shift) & 0xffU;
        *hash *= kFnvPrime;
    }
}

void mix_i64(std::uint64_t* hash, std::int64_t value) noexcept {
    mix_u64(hash, static_cast<std::uint64_t>(value));
}

void mix_string(std::uint64_t* hash, const std::string& value) noexcept {
    for (const auto ch : value) {
        *hash ^= static_cast<unsigned char>(ch);
        *hash *= kFnvPrime;
    }
}

[[nodiscard]] bool is_filled_maker_status(
    trading_engine::execution::MakerQuoteStatus status
) noexcept {
    using trading_engine::execution::MakerQuoteStatus;
    return status == MakerQuoteStatus::Filled ||
           status == MakerQuoteStatus::PartiallyFilled;
}

[[nodiscard]] Side paper_side_from_quote_side(
    trading_engine::execution::QuoteSide side
) noexcept {
    return side == trading_engine::execution::QuoteSide::Ask
        ? Side::Sell
        : Side::Buy;
}

}  // namespace

std::uint64_t compute_paper_fill_hash(const PaperFill& fill) noexcept {
    auto hash = kFnvOffset;
    mix_u64(&hash, fill.report_id);
    mix_u64(&hash, fill.plan_id);
    mix_u64(&hash, fill.order_id);
    mix_u64(&hash, fill.quote_id);
    mix_u64(&hash, fill.approved_quote_id);
    mix_u64(&hash, fill.quote_group_id);
    mix_u64(&hash, fill.asset_index);
    mix_string(&hash, fill.asset_id);
    mix_u64(&hash, static_cast<std::uint8_t>(fill.side));
    mix_u64(&hash, static_cast<std::uint8_t>(fill.liquidity_role));
    mix_i64(&hash, fill.qty_lots);
    mix_i64(&hash, fill.fill_price_tick);
    mix_i64(&hash, fill.fee_tick);
    mix_u64(&hash, fill.ts_ns);
    return hash == 0 ? 1 : hash;
}

MakerFillApplicationResult MakerFillApplication::from_report(
    const trading_engine::execution::MakerExecutionReport& report,
    std::int64_t fee_tick
) const {
    if (report.liquidity_role != FillLiquidityRole::Maker) {
        return {.reason = "report is not maker liquidity"};
    }
    if (!is_filled_maker_status(report.status) || report.filled_qty_lots <= 0) {
        return {.reason = "maker report has no filled quantity"};
    }
    if (report.avg_fill_price_tick < 0 || fee_tick < 0 ||
        report.asset_id.empty()) {
        return {.reason = "invalid maker fill report"};
    }

    PaperFill fill;
    fill.report_id = report.report_id != 0
        ? report.report_id
        : trading_engine::execution::compute_maker_execution_report_hash(report);
    fill.quote_id = report.quote_id;
    fill.approved_quote_id = report.approved_quote_id;
    fill.quote_group_id = report.quote_group_id;
    fill.asset_index = report.asset_index;
    fill.asset_id = report.asset_id;
    fill.side = paper_side_from_quote_side(report.side);
    fill.liquidity_role = FillLiquidityRole::Maker;
    fill.qty_lots = report.filled_qty_lots;
    fill.fill_price_tick = report.avg_fill_price_tick;
    fill.fee_tick = fee_tick;
    fill.ts_ns = report.recv_ts_ns != 0 ? report.recv_ts_ns : report.exchange_ts_ns;
    fill.idempotency_hash = compute_paper_fill_hash(fill);
    fill.fill_id = fill.idempotency_hash;

    return {
        .has_fill = true,
        .fill = std::move(fill),
        .reason = {}
    };
}

}  // namespace trading_engine::paper
