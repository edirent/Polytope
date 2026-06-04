#include "engine/execution/adapter/PaperMakerFillModel.h"

#include <algorithm>
#include <cmath>

namespace trading_engine::execution {

namespace {

[[nodiscard]] std::int64_t level_size_lots(
    const state::PriceLevel& level
) noexcept {
    if (!std::isfinite(level.size) || level.size <= 0.0) {
        return 0;
    }
    return static_cast<std::int64_t>(std::floor(level.size));
}

[[nodiscard]] std::int64_t remaining_for(
    const PaperMakerQuote& quote,
    QuoteSide side
) noexcept {
    if (side == QuoteSide::Bid && quote.has_bid) {
        return std::max<std::int64_t>(
            0,
            quote.bid.quantity_lots - quote.filled_bid_qty_lots
        );
    }
    if (side == QuoteSide::Ask && quote.has_ask) {
        return std::max<std::int64_t>(
            0,
            quote.ask.quantity_lots - quote.filled_ask_qty_lots
        );
    }
    return 0;
}

[[nodiscard]] MakerFillResult make_fill(
    const PaperMakerQuote& quote,
    QuoteSide side,
    std::int64_t qty,
    std::int64_t price,
    std::uint64_t ts,
    const char* reason
) {
    MakerFillResult out;
    out.filled = qty > 0;
    out.quote_id = quote.quote_id;
    out.approved_quote_id = quote.approved_quote_id;
    out.quote_group_id = quote.quote_group_id;
    out.asset_index = quote.asset_index;
    out.asset_id = quote.asset_id;
    out.side = side;
    out.filled_qty_lots = qty;
    out.fill_price_tick = price;
    out.ts_ns = ts;
    out.reason = reason;
    return out;
}

[[nodiscard]] std::int64_t capped_qty(
    std::int64_t remaining,
    std::int64_t evidence_qty,
    bool allow_partial_fills,
    std::int64_t max_fill_qty_per_trade
) noexcept {
    if (remaining <= 0 || evidence_qty <= 0) {
        return 0;
    }
    auto qty = allow_partial_fills ? std::min(remaining, evidence_qty)
                                   : (evidence_qty >= remaining ? remaining : 0);
    if (max_fill_qty_per_trade > 0) {
        qty = std::min(qty, max_fill_qty_per_trade);
    }
    return qty;
}

}  // namespace

std::vector<MakerFillResult> PaperMakerFillModel::evaluate(
    const PaperMakerQuote& quote,
    const PaperMakerMarketEvent& event,
    PaperMakerFillMode mode,
    bool allow_partial_fills,
    std::int64_t max_fill_qty_per_trade,
    std::uint64_t queue_min_rest_ns
) const {
    std::vector<MakerFillResult> fills;
    if (mode == PaperMakerFillMode::NoFill ||
        quote.status == MakerQuoteStatus::Cancelled ||
        quote.status == MakerQuoteStatus::Expired ||
        quote.status == MakerQuoteStatus::Filled ||
        (quote.expires_at_ns != 0 && quote.expires_at_ns <= event.ts_ns)) {
        return fills;
    }

    if (mode == PaperMakerFillMode::Conservative) {
        if (!event.has_trade || event.trade_qty_lots <= 0 ||
            event.trade_price_tick <= 0) {
            return fills;
        }
        if (quote.has_bid && event.trade_price_tick <= quote.bid.price_tick) {
            const auto qty = capped_qty(
                remaining_for(quote, QuoteSide::Bid),
                event.trade_qty_lots,
                allow_partial_fills,
                max_fill_qty_per_trade
            );
            if (qty > 0) {
                fills.push_back(make_fill(
                    quote,
                    QuoteSide::Bid,
                    qty,
                    quote.bid.price_tick,
                    event.ts_ns,
                    "trade_through_bid"
                ));
            }
        }
        if (quote.has_ask && event.trade_price_tick >= quote.ask.price_tick) {
            const auto qty = capped_qty(
                remaining_for(quote, QuoteSide::Ask),
                event.trade_qty_lots,
                allow_partial_fills,
                max_fill_qty_per_trade
            );
            if (qty > 0) {
                fills.push_back(make_fill(
                    quote,
                    QuoteSide::Ask,
                    qty,
                    quote.ask.price_tick,
                    event.ts_ns,
                    "trade_through_ask"
                ));
            }
        }
        return fills;
    }

    if (mode == PaperMakerFillMode::BookCross) {
        if (!event.depth) {
            return fills;
        }
        if (quote.has_bid && event.depth->ask_count > 0 &&
            event.depth->asks[0].price_tick > 0 &&
            event.depth->asks[0].price_tick <= quote.bid.price_tick) {
            const auto qty = capped_qty(
                remaining_for(quote, QuoteSide::Bid),
                level_size_lots(event.depth->asks[0]),
                allow_partial_fills,
                max_fill_qty_per_trade
            );
            if (qty > 0) {
                fills.push_back(make_fill(
                    quote,
                    QuoteSide::Bid,
                    qty,
                    quote.bid.price_tick,
                    event.ts_ns,
                    "book_cross_bid"
                ));
            }
        }
        if (quote.has_ask && event.depth->bid_count > 0 &&
            event.depth->bids[0].price_tick > 0 &&
            event.depth->bids[0].price_tick >= quote.ask.price_tick) {
            const auto qty = capped_qty(
                remaining_for(quote, QuoteSide::Ask),
                level_size_lots(event.depth->bids[0]),
                allow_partial_fills,
                max_fill_qty_per_trade
            );
            if (qty > 0) {
                fills.push_back(make_fill(
                    quote,
                    QuoteSide::Ask,
                    qty,
                    quote.ask.price_tick,
                    event.ts_ns,
                    "book_cross_ask"
                ));
            }
        }
    }
    if (mode == PaperMakerFillMode::QueueAware) {
        if (!event.depth ||
            event.ts_ns < quote.created_ts_ns ||
            event.ts_ns - quote.created_ts_ns < queue_min_rest_ns) {
            return fills;
        }
        if (quote.has_bid && event.depth->ask_count > 0 &&
            event.depth->asks[0].price_tick > 0 &&
            event.depth->asks[0].price_tick < quote.bid.price_tick) {
            const auto qty = capped_qty(
                remaining_for(quote, QuoteSide::Bid),
                level_size_lots(event.depth->asks[0]),
                allow_partial_fills,
                max_fill_qty_per_trade
            );
            if (qty > 0) {
                fills.push_back(make_fill(
                    quote,
                    QuoteSide::Bid,
                    qty,
                    quote.bid.price_tick,
                    event.ts_ns,
                    "queue_aware_cross_bid"
                ));
            }
        }
        if (quote.has_ask && event.depth->bid_count > 0 &&
            event.depth->bids[0].price_tick > 0 &&
            event.depth->bids[0].price_tick > quote.ask.price_tick) {
            const auto qty = capped_qty(
                remaining_for(quote, QuoteSide::Ask),
                level_size_lots(event.depth->bids[0]),
                allow_partial_fills,
                max_fill_qty_per_trade
            );
            if (qty > 0) {
                fills.push_back(make_fill(
                    quote,
                    QuoteSide::Ask,
                    qty,
                    quote.ask.price_tick,
                    event.ts_ns,
                    "queue_aware_cross_ask"
                ));
            }
        }
    }
    if (mode == PaperMakerFillMode::MidCross) {
        if (!event.depth || event.depth->bid_count == 0 ||
            event.depth->ask_count == 0 ||
            event.depth->bids[0].price_tick <= 0 ||
            event.depth->asks[0].price_tick <= 0) {
            return fills;
        }
        const auto mid_tick =
            (event.depth->bids[0].price_tick +
             event.depth->asks[0].price_tick) /
            2;
        if (quote.has_bid && mid_tick <= quote.bid.price_tick) {
            const auto qty = capped_qty(
                remaining_for(quote, QuoteSide::Bid),
                std::max<std::int64_t>(1, level_size_lots(event.depth->asks[0])),
                allow_partial_fills,
                max_fill_qty_per_trade
            );
            if (qty > 0) {
                fills.push_back(make_fill(
                    quote,
                    QuoteSide::Bid,
                    qty,
                    quote.bid.price_tick,
                    event.ts_ns,
                    "mid_cross_bid"
                ));
            }
        }
        if (quote.has_ask && mid_tick >= quote.ask.price_tick) {
            const auto qty = capped_qty(
                remaining_for(quote, QuoteSide::Ask),
                std::max<std::int64_t>(1, level_size_lots(event.depth->bids[0])),
                allow_partial_fills,
                max_fill_qty_per_trade
            );
            if (qty > 0) {
                fills.push_back(make_fill(
                    quote,
                    QuoteSide::Ask,
                    qty,
                    quote.ask.price_tick,
                    event.ts_ns,
                    "mid_cross_ask"
                ));
            }
        }
    }
    return fills;
}

}  // namespace trading_engine::execution
