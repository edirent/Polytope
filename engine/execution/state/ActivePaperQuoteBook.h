#pragma once

#include "engine/execution/public/MakerExecutionTypes.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace trading_engine::execution {

class ActivePaperQuoteBook {
public:
    bool add_or_replace(const risk::ApprovedQuote& quote, std::uint64_t now_ns);

    bool cancel_by_group(std::uint64_t quote_group_id, std::uint64_t now_ns);
    bool cancel_by_quote_id(std::uint64_t quote_id, std::uint64_t now_ns);

    void expire_old(std::uint64_t now_ns);

    bool apply_fill(
        std::uint64_t quote_id,
        QuoteSide side,
        std::int64_t qty_lots
    );

    [[nodiscard]] std::vector<PaperMakerQuote> active_quotes_for_asset(
        std::uint32_t asset_index
    ) const;

    [[nodiscard]] const PaperMakerQuote* find_by_quote_id(
        std::uint64_t quote_id
    ) const;

    [[nodiscard]] std::size_t active_quote_count() const;
    [[nodiscard]] std::size_t duplicate_ignored_count() const;
    [[nodiscard]] std::size_t cancelled_quote_count() const;
    [[nodiscard]] std::size_t expired_quote_count() const;
    [[nodiscard]] std::size_t replaced_quote_count() const;
    [[nodiscard]] bool last_add_replaced() const;

private:
    [[nodiscard]] PaperMakerQuote make_quote(
        const risk::ApprovedQuote& quote,
        std::uint64_t now_ns
    ) const;

    std::unordered_map<std::uint64_t, PaperMakerQuote> quotes_by_id_;
    std::unordered_map<std::uint64_t, std::uint64_t> quote_id_by_group_;
    std::unordered_map<std::uint64_t, std::uint64_t>
        quote_id_by_idempotency_hash_;

    std::size_t duplicate_ignored_count_ = 0;
    std::size_t cancelled_quote_count_ = 0;
    std::size_t expired_quote_count_ = 0;
    std::size_t replaced_quote_count_ = 0;
    bool last_add_replaced_ = false;
};

}  // namespace trading_engine::execution
