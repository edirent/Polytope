#include "engine/execution/adapter/PaperMakerExecutionAdapter.h"

namespace trading_engine::execution {

PaperMakerExecutionAdapter::PaperMakerExecutionAdapter(PaperMakerFillMode mode)
    : config_(PaperMakerExecutionConfig{.fill_mode = mode}) {}

PaperMakerExecutionAdapter::PaperMakerExecutionAdapter(
    PaperMakerExecutionConfig config
) : config_(config) {}

MakerSubmitResult PaperMakerExecutionAdapter::submit_approved_quote(
    const risk::ApprovedQuote& quote,
    std::uint64_t now_ns
) {
    MakerSubmitResult result;
    if (!quote.has_bid && !quote.has_ask) {
        result.error = "approved quote has no active sides";
        return result;
    }
    if (quote.expires_at_ns != 0 && quote.expires_at_ns <= now_ns) {
        result.error = "approved quote expired";
        return result;
    }

    const auto before_duplicates = quote_book_.duplicate_ignored_count();
    const auto ok = quote_book_.add_or_replace(quote, now_ns);
    result.duplicate_ignored =
        quote_book_.duplicate_ignored_count() > before_duplicates;
    result.replaced = quote_book_.last_add_replaced();
    result.ok = ok || result.duplicate_ignored;
    result.quote_id = compute_paper_maker_quote_id(quote);
    if (!result.ok) {
        result.error = "failed to add paper maker quote";
    }
    return result;
}

MakerCancelResult PaperMakerExecutionAdapter::cancel_quote_group(
    std::uint64_t quote_group_id,
    std::uint64_t now_ns
) {
    MakerCancelResult result;
    result.ok = quote_book_.cancel_by_group(quote_group_id, now_ns);
    if (!result.ok) {
        result.error = "quote group not active";
    }
    return result;
}

std::vector<MakerExecutionReport> PaperMakerExecutionAdapter::on_market_event(
    const PaperMakerMarketEvent& event
) {
    quote_book_.expire_old(event.ts_ns);
    std::vector<MakerExecutionReport> reports;
    for (const auto& quote : quote_book_.active_quotes_for_asset(event.asset_index)) {
        const auto fills = fill_model_.evaluate(
            quote,
            event,
            config_.fill_mode,
            config_.allow_partial_fills,
            config_.max_fill_qty_per_trade,
            config_.queue_min_rest_ns
        );
        for (const auto& fill : fills) {
            const auto remaining_before =
                fill.side == QuoteSide::Bid
                    ? quote.bid.quantity_lots - quote.filled_bid_qty_lots
                    : quote.ask.quantity_lots - quote.filled_ask_qty_lots;
            const auto remaining_after =
                remaining_before > fill.filled_qty_lots
                    ? remaining_before - fill.filled_qty_lots
                    : 0;
            if (quote_book_.apply_fill(
                    fill.quote_id,
                    fill.side,
                    fill.filled_qty_lots
                )) {
                reports.push_back(make_report(fill, remaining_after));
            }
        }
    }
    return reports;
}

void PaperMakerExecutionAdapter::expire_old(std::uint64_t now_ns) {
    quote_book_.expire_old(now_ns);
}

const ActivePaperQuoteBook& PaperMakerExecutionAdapter::quote_book()
    const noexcept {
    return quote_book_;
}

PaperMakerFillMode PaperMakerExecutionAdapter::fill_mode() const noexcept {
    return config_.fill_mode;
}

MakerExecutionReport PaperMakerExecutionAdapter::make_report(
    const MakerFillResult& fill,
    std::int64_t remaining_qty_lots
) const {
    MakerExecutionReport report;
    report.quote_id = fill.quote_id;
    report.approved_quote_id = fill.approved_quote_id;
    report.quote_group_id = fill.quote_group_id;
    report.asset_index = fill.asset_index;
    report.asset_id = fill.asset_id;
    report.side = fill.side;
    report.status = remaining_qty_lots == 0 ? MakerQuoteStatus::Filled
                                            : MakerQuoteStatus::PartiallyFilled;
    report.liquidity_role = FillLiquidityRole::Maker;
    report.filled_qty_lots = fill.filled_qty_lots;
    report.avg_fill_price_tick = fill.fill_price_tick;
    report.remaining_qty_lots = remaining_qty_lots;
    report.exchange_ts_ns = fill.ts_ns;
    report.recv_ts_ns = fill.ts_ns;
    report.reason = fill.reason;
    report.report_id = compute_maker_execution_report_hash(report);
    return report;
}

}  // namespace trading_engine::execution
