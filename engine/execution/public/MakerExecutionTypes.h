#pragma once

#include "engine/risk/public/ApprovedQuote.h"
#include "engine/strategy/market_making/public/MarketMakingTypes.h"

#include <cstdint>
#include <string>

namespace trading_engine::execution {

using QuoteLeg = risk::QuoteLeg;
using QuoteSide = strategy::market_making::QuoteSide;

enum class PaperMakerFillMode : std::uint8_t {
    NoFill,
    Conservative,
    BookCross,
    MidCross,
    QueueAware
};

enum class MakerQuoteStatus : std::uint8_t {
    Created,
    ActivePaper,
    PartiallyFilled,
    Filled,
    CancelRequested,
    Cancelled,
    Expired,
    Replaced,
    Failed
};

enum class FillLiquidityRole : std::uint8_t {
    Maker,
    Taker,
    Unknown
};

struct PaperMakerQuote {
    std::uint64_t quote_id = 0;
    std::uint64_t approved_quote_id = 0;
    std::uint64_t quote_intent_id = 0;
    std::uint64_t quote_group_id = 0;

    std::uint32_t asset_index = 0;
    std::string asset_id;

    bool has_bid = false;
    bool has_ask = false;

    QuoteLeg bid;
    QuoteLeg ask;

    MakerQuoteStatus status = MakerQuoteStatus::Created;

    std::int64_t filled_bid_qty_lots = 0;
    std::int64_t filled_ask_qty_lots = 0;

    std::uint64_t created_ts_ns = 0;
    std::uint64_t expires_at_ns = 0;

    std::uint64_t idempotency_hash = 0;
};

struct MakerExecutionReport {
    std::uint64_t report_id = 0;
    std::uint64_t quote_id = 0;
    std::uint64_t approved_quote_id = 0;
    std::uint64_t quote_group_id = 0;

    std::uint32_t asset_index = 0;
    std::string asset_id;

    QuoteSide side = QuoteSide::Bid;

    MakerQuoteStatus status = MakerQuoteStatus::ActivePaper;
    FillLiquidityRole liquidity_role = FillLiquidityRole::Maker;

    std::int64_t filled_qty_lots = 0;
    std::int64_t avg_fill_price_tick = 0;
    std::int64_t remaining_qty_lots = 0;

    std::uint64_t exchange_ts_ns = 0;
    std::uint64_t recv_ts_ns = 0;

    std::string reason;
};

struct MakerSubmitResult {
    bool ok = false;
    bool duplicate_ignored = false;
    bool replaced = false;
    std::uint64_t quote_id = 0;
    std::string error;
};

struct MakerCancelResult {
    bool ok = false;
    std::uint64_t quote_id = 0;
    std::string error;
};

[[nodiscard]] std::uint64_t compute_paper_maker_quote_id(
    const risk::ApprovedQuote& quote
) noexcept;

[[nodiscard]] std::uint64_t compute_maker_execution_report_hash(
    const MakerExecutionReport& report
) noexcept;

}  // namespace trading_engine::execution
