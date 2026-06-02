#include "engine/execution/state/ActivePaperQuoteBook.h"

#include "engine/risk/public/ApprovedQuote.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::execution::ActivePaperQuoteBook;
using trading_engine::execution::QuoteSide;
using trading_engine::risk::ApprovedQuote;
using trading_engine::risk::compute_approved_quote_hash;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

template <typename Actual, typename Expected>
void expect_equal(
    const Actual& actual,
    const Expected& expected,
    const std::string& field
) {
    if (!(actual == expected)) {
        fail("mismatch: " + field);
    }
}

ApprovedQuote approved_quote(
    std::uint64_t quote_group_id = 301,
    std::uint64_t idempotency_hash = 401,
    std::uint32_t asset_index = 7
) {
    ApprovedQuote quote;
    quote.quote_intent_id = 201 + idempotency_hash;
    quote.quote_group_id = quote_group_id;
    quote.has_bid = true;
    quote.has_ask = true;

    quote.bid.market_id = "market";
    quote.bid.asset_id = "asset-" + std::to_string(asset_index);
    quote.bid.market_index = 3;
    quote.bid.asset_index = asset_index;
    quote.bid.side = QuoteSide::Bid;
    quote.bid.price_tick = 495'000;
    quote.bid.quantity_lots = 10;
    quote.bid.fair_value_tick = 500'000;
    quote.bid.edge_to_fair_tick = 5'000;
    quote.bid.book_version = 11;
    quote.bid.snapshot_version_hash = 601;

    quote.ask = quote.bid;
    quote.ask.side = QuoteSide::Ask;
    quote.ask.price_tick = 505'000;
    quote.ask.edge_to_fair_tick = 5'000;

    quote.approved_ts_ns = 1'000;
    quote.expires_at_ns = 5'000;
    quote.idempotency_hash = idempotency_hash;
    quote.policy_hash = 501;
    quote.snapshot_version_hash = 601;
    quote.approved_quote_id = compute_approved_quote_hash(quote);
    return quote;
}

void ActivePaperQuoteBook_AddsApprovedQuote() {
    ActivePaperQuoteBook book;
    const auto quote = approved_quote();

    expect_true(book.add_or_replace(quote, 1'100), "add approved quote");
    expect_equal(book.active_quote_count(), static_cast<std::size_t>(1), "count");
    const auto active = book.active_quotes_for_asset(7);
    expect_equal(active.size(), static_cast<std::size_t>(1), "asset query");
    expect_equal(active[0].quote_group_id, 301ULL, "group id");
}

void ActivePaperQuoteBook_ReplacesSameQuoteGroup() {
    ActivePaperQuoteBook book;
    const auto first = approved_quote(301, 401, 7);
    auto second = approved_quote(301, 402, 7);
    second.bid.price_tick = 496'000;
    second.approved_quote_id = compute_approved_quote_hash(second);

    expect_true(book.add_or_replace(first, 1'100), "add first");
    expect_true(book.add_or_replace(second, 1'200), "replace second");
    expect_equal(book.active_quote_count(), static_cast<std::size_t>(1), "count");
    expect_equal(book.replaced_quote_count(), static_cast<std::size_t>(1), "replaced");
    const auto active = book.active_quotes_for_asset(7);
    expect_equal(active[0].bid.price_tick, 496'000LL, "replacement price");
}

void ActivePaperQuoteBook_IgnoresDuplicateIdempotencyHash() {
    ActivePaperQuoteBook book;
    const auto quote = approved_quote();

    expect_true(book.add_or_replace(quote, 1'100), "add first");
    expect_true(!book.add_or_replace(quote, 1'200), "duplicate ignored");
    expect_equal(book.active_quote_count(), static_cast<std::size_t>(1), "count");
    expect_equal(
        book.duplicate_ignored_count(),
        static_cast<std::size_t>(1),
        "duplicates"
    );
}

void ActivePaperQuoteBook_CancelsQuoteGroup() {
    ActivePaperQuoteBook book;
    const auto quote = approved_quote();

    expect_true(book.add_or_replace(quote, 1'100), "add quote");
    expect_true(book.cancel_by_group(quote.quote_group_id, 1'200), "cancel group");
    expect_equal(book.active_quote_count(), static_cast<std::size_t>(0), "count");
    expect_equal(book.cancelled_quote_count(), static_cast<std::size_t>(1), "cancelled");
}

void ActivePaperQuoteBook_ExpiresOldQuotes() {
    ActivePaperQuoteBook book;
    const auto quote = approved_quote();

    expect_true(book.add_or_replace(quote, 1'100), "add quote");
    book.expire_old(quote.expires_at_ns);
    expect_equal(book.active_quote_count(), static_cast<std::size_t>(0), "count");
    expect_equal(book.expired_quote_count(), static_cast<std::size_t>(1), "expired");
}

void ActivePaperQuoteBook_QueriesByAsset() {
    ActivePaperQuoteBook book;
    expect_true(book.add_or_replace(approved_quote(301, 401, 7), 1'100), "add 7");
    expect_true(book.add_or_replace(approved_quote(302, 402, 8), 1'100), "add 8");

    const auto asset7 = book.active_quotes_for_asset(7);
    const auto asset8 = book.active_quotes_for_asset(8);
    expect_equal(asset7.size(), static_cast<std::size_t>(1), "asset 7");
    expect_equal(asset8.size(), static_cast<std::size_t>(1), "asset 8");
    expect_equal(asset7[0].asset_index, 7U, "asset 7 index");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"ActivePaperQuoteBook_AddsApprovedQuote", &ActivePaperQuoteBook_AddsApprovedQuote},
        {"ActivePaperQuoteBook_ReplacesSameQuoteGroup", &ActivePaperQuoteBook_ReplacesSameQuoteGroup},
        {"ActivePaperQuoteBook_IgnoresDuplicateIdempotencyHash", &ActivePaperQuoteBook_IgnoresDuplicateIdempotencyHash},
        {"ActivePaperQuoteBook_CancelsQuoteGroup", &ActivePaperQuoteBook_CancelsQuoteGroup},
        {"ActivePaperQuoteBook_ExpiresOldQuotes", &ActivePaperQuoteBook_ExpiresOldQuotes},
        {"ActivePaperQuoteBook_QueriesByAsset", &ActivePaperQuoteBook_QueriesByAsset},
    };
    return test_map;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "expected test name\n";
        return 1;
    }
    const auto it = tests().find(argv[1]);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << argv[1] << "\n";
        return 1;
    }
    try {
        it->second();
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
    return 0;
}
