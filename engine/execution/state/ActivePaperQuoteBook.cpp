#include "engine/execution/state/ActivePaperQuoteBook.h"

#include <algorithm>

namespace trading_engine::execution {

namespace {

[[nodiscard]] bool quote_fully_filled(const PaperMakerQuote& quote) noexcept {
    const bool bid_done = !quote.has_bid ||
                          quote.filled_bid_qty_lots >= quote.bid.quantity_lots;
    const bool ask_done = !quote.has_ask ||
                          quote.filled_ask_qty_lots >= quote.ask.quantity_lots;
    return bid_done && ask_done;
}

}  // namespace

PaperMakerQuote ActivePaperQuoteBook::make_quote(
    const risk::ApprovedQuote& quote,
    std::uint64_t now_ns
) const {
    PaperMakerQuote out;
    out.quote_id = compute_paper_maker_quote_id(quote);
    out.approved_quote_id = quote.approved_quote_id;
    out.quote_intent_id = quote.quote_intent_id;
    out.quote_group_id = quote.quote_group_id;
    out.has_bid = quote.has_bid;
    out.has_ask = quote.has_ask;
    out.bid = quote.bid;
    out.ask = quote.ask;
    if (quote.has_bid) {
        out.asset_index = quote.bid.asset_index;
        out.asset_id = quote.bid.asset_id;
    } else if (quote.has_ask) {
        out.asset_index = quote.ask.asset_index;
        out.asset_id = quote.ask.asset_id;
    }
    out.status = MakerQuoteStatus::ActivePaper;
    out.created_ts_ns = now_ns;
    out.expires_at_ns = quote.expires_at_ns;
    out.idempotency_hash = quote.idempotency_hash;
    return out;
}

bool ActivePaperQuoteBook::add_or_replace(
    const risk::ApprovedQuote& quote,
    std::uint64_t now_ns
) {
    last_add_replaced_ = false;
    if (!quote.has_bid && !quote.has_ask) {
        return false;
    }
    if (quote.idempotency_hash != 0 &&
        quote_id_by_idempotency_hash_.contains(quote.idempotency_hash)) {
        ++duplicate_ignored_count_;
        return false;
    }

    const auto existing = quote_id_by_group_.find(quote.quote_group_id);
    if (existing != quote_id_by_group_.end()) {
        const auto old_quote = quotes_by_id_.find(existing->second);
        if (old_quote != quotes_by_id_.end() &&
            old_quote->second.idempotency_hash != 0) {
            quote_id_by_idempotency_hash_.erase(
                old_quote->second.idempotency_hash
            );
        }
        quotes_by_id_.erase(existing->second);
        quote_id_by_group_.erase(existing);
        ++replaced_quote_count_;
        last_add_replaced_ = true;
    }

    auto paper_quote = make_quote(quote, now_ns);
    if (paper_quote.quote_id == 0) {
        return false;
    }
    quote_id_by_group_[paper_quote.quote_group_id] = paper_quote.quote_id;
    if (paper_quote.idempotency_hash != 0) {
        quote_id_by_idempotency_hash_[paper_quote.idempotency_hash] =
            paper_quote.quote_id;
    }
    quotes_by_id_[paper_quote.quote_id] = paper_quote;
    return true;
}

bool ActivePaperQuoteBook::cancel_by_group(
    std::uint64_t quote_group_id,
    std::uint64_t
) {
    const auto it = quote_id_by_group_.find(quote_group_id);
    if (it == quote_id_by_group_.end()) {
        return false;
    }
    const auto quote_id = it->second;
    quote_id_by_group_.erase(it);
    const auto quote = quotes_by_id_.find(quote_id);
    if (quote != quotes_by_id_.end() && quote->second.idempotency_hash != 0) {
        quote_id_by_idempotency_hash_.erase(quote->second.idempotency_hash);
    }
    quotes_by_id_.erase(quote_id);
    ++cancelled_quote_count_;
    return true;
}

bool ActivePaperQuoteBook::cancel_by_quote_id(
    std::uint64_t quote_id,
    std::uint64_t
) {
    const auto it = quotes_by_id_.find(quote_id);
    if (it == quotes_by_id_.end()) {
        return false;
    }
    quote_id_by_group_.erase(it->second.quote_group_id);
    if (it->second.idempotency_hash != 0) {
        quote_id_by_idempotency_hash_.erase(it->second.idempotency_hash);
    }
    quotes_by_id_.erase(it);
    ++cancelled_quote_count_;
    return true;
}

void ActivePaperQuoteBook::expire_old(std::uint64_t now_ns) {
    std::vector<std::uint64_t> expired;
    for (const auto& [quote_id, quote] : quotes_by_id_) {
        if (quote.expires_at_ns != 0 && quote.expires_at_ns <= now_ns) {
            expired.push_back(quote_id);
        }
    }
    std::sort(expired.begin(), expired.end());
    for (const auto quote_id : expired) {
        const auto it = quotes_by_id_.find(quote_id);
        if (it == quotes_by_id_.end()) {
            continue;
        }
        quote_id_by_group_.erase(it->second.quote_group_id);
        if (it->second.idempotency_hash != 0) {
            quote_id_by_idempotency_hash_.erase(it->second.idempotency_hash);
        }
        quotes_by_id_.erase(it);
        ++expired_quote_count_;
    }
}

bool ActivePaperQuoteBook::apply_fill(
    std::uint64_t quote_id,
    QuoteSide side,
    std::int64_t qty_lots
) {
    if (qty_lots <= 0) {
        return false;
    }
    const auto it = quotes_by_id_.find(quote_id);
    if (it == quotes_by_id_.end()) {
        return false;
    }
    auto& quote = it->second;
    if (side == QuoteSide::Bid && quote.has_bid) {
        quote.filled_bid_qty_lots =
            std::min(quote.bid.quantity_lots, quote.filled_bid_qty_lots + qty_lots);
    } else if (side == QuoteSide::Ask && quote.has_ask) {
        quote.filled_ask_qty_lots =
            std::min(quote.ask.quantity_lots, quote.filled_ask_qty_lots + qty_lots);
    } else {
        return false;
    }
    quote.status = quote_fully_filled(quote) ? MakerQuoteStatus::Filled
                                             : MakerQuoteStatus::PartiallyFilled;
    if (quote.status == MakerQuoteStatus::Filled) {
        quote_id_by_group_.erase(quote.quote_group_id);
        if (quote.idempotency_hash != 0) {
            quote_id_by_idempotency_hash_.erase(quote.idempotency_hash);
        }
        quotes_by_id_.erase(it);
    }
    return true;
}

std::vector<PaperMakerQuote> ActivePaperQuoteBook::active_quotes_for_asset(
    std::uint32_t asset_index
) const {
    std::vector<PaperMakerQuote> out;
    for (const auto& [_, quote] : quotes_by_id_) {
        if (quote.asset_index == asset_index) {
            out.push_back(quote);
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.quote_id < rhs.quote_id;
    });
    return out;
}

const PaperMakerQuote* ActivePaperQuoteBook::find_by_quote_id(
    std::uint64_t quote_id
) const {
    const auto it = quotes_by_id_.find(quote_id);
    return it == quotes_by_id_.end() ? nullptr : &it->second;
}

std::size_t ActivePaperQuoteBook::active_quote_count() const {
    return quotes_by_id_.size();
}

std::size_t ActivePaperQuoteBook::duplicate_ignored_count() const {
    return duplicate_ignored_count_;
}

std::size_t ActivePaperQuoteBook::cancelled_quote_count() const {
    return cancelled_quote_count_;
}

std::size_t ActivePaperQuoteBook::expired_quote_count() const {
    return expired_quote_count_;
}

std::size_t ActivePaperQuoteBook::replaced_quote_count() const {
    return replaced_quote_count_;
}

bool ActivePaperQuoteBook::last_add_replaced() const {
    return last_add_replaced_;
}

}  // namespace trading_engine::execution
