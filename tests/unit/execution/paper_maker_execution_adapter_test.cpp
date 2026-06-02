#include "engine/execution/adapter/PaperMakerExecutionAdapter.h"

#include "engine/risk/public/ApprovedQuote.h"
#include "engine/state/book/DepthPrefix.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::execution::FillLiquidityRole;
using trading_engine::execution::MakerQuoteStatus;
using trading_engine::execution::PaperMakerExecutionAdapter;
using trading_engine::execution::PaperMakerFillMode;
using trading_engine::execution::PaperMakerMarketEvent;
using trading_engine::execution::QuoteSide;
using trading_engine::risk::ApprovedQuote;
using trading_engine::risk::compute_approved_quote_hash;
using trading_engine::state::MarketDepthView;
using trading_engine::state::PriceLevel;

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

MarketDepthView depth_view(
    std::int64_t bid_tick,
    std::int64_t ask_tick,
    double bid_size = 10.0,
    double ask_size = 10.0
) {
    MarketDepthView depth;
    depth.asset_index = 7;
    depth.book_version = 11;
    depth.snapshot_version_hash = 601;
    depth.last_ws_recv_ns = 1'000;
    depth.usable_for_depth = true;
    depth.bid_count = 1;
    depth.ask_count = 1;
    depth.bids[0] = PriceLevel{
        .price_tick = bid_tick,
        .price = static_cast<double>(bid_tick) / 1'000'000.0,
        .size = bid_size
    };
    depth.asks[0] = PriceLevel{
        .price_tick = ask_tick,
        .price = static_cast<double>(ask_tick) / 1'000'000.0,
        .size = ask_size
    };
    trading_engine::state::build_depth_prefix(
        depth.bids,
        depth.bid_count,
        depth.asks,
        depth.ask_count,
        &depth.prefix
    );
    return depth;
}

ApprovedQuote approved_quote(
    std::uint64_t quote_group_id = 301,
    std::uint64_t idempotency_hash = 401
) {
    ApprovedQuote quote;
    quote.quote_intent_id = 201 + idempotency_hash;
    quote.quote_group_id = quote_group_id;
    quote.has_bid = true;
    quote.has_ask = true;

    quote.bid.market_id = "market";
    quote.bid.asset_id = "asset-7";
    quote.bid.market_index = 3;
    quote.bid.asset_index = 7;
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

    quote.approved_ts_ns = 1'000;
    quote.expires_at_ns = 5'000;
    quote.idempotency_hash = idempotency_hash;
    quote.policy_hash = 501;
    quote.snapshot_version_hash = 601;
    quote.approved_quote_id = compute_approved_quote_hash(quote);
    return quote;
}

ApprovedQuote bid_only_quote() {
    auto quote = approved_quote();
    quote.has_ask = false;
    quote.approved_quote_id = compute_approved_quote_hash(quote);
    return quote;
}

PaperMakerMarketEvent conservative_trade(std::int64_t price_tick, std::int64_t qty) {
    PaperMakerMarketEvent event;
    event.ts_ns = 2'000;
    event.asset_index = 7;
    event.has_trade = true;
    event.trade_price_tick = price_tick;
    event.trade_qty_lots = qty;
    return event;
}

void PaperMaker_EmitsMakerExecutionReport() {
    PaperMakerExecutionAdapter adapter(PaperMakerFillMode::Conservative);
    const auto quote = bid_only_quote();

    const auto submit = adapter.submit_approved_quote(quote, 1'100);
    expect_true(submit.ok, "submit");

    const auto reports = adapter.on_market_event(conservative_trade(494'000, 10));
    expect_equal(reports.size(), static_cast<std::size_t>(1), "report count");
    expect_equal(reports[0].approved_quote_id, quote.approved_quote_id, "approved id");
    expect_equal(reports[0].quote_group_id, quote.quote_group_id, "group id");
    expect_equal(reports[0].liquidity_role, FillLiquidityRole::Maker, "liquidity");
    expect_equal(reports[0].filled_qty_lots, 10LL, "qty");
    expect_equal(reports[0].avg_fill_price_tick, 495'000LL, "price");
    expect_equal(reports[0].remaining_qty_lots, 0LL, "remaining");
    expect_equal(reports[0].status, MakerQuoteStatus::Filled, "status");
    expect_true(reports[0].report_id != 0, "report hash");
    expect_equal(adapter.quote_book().active_quote_count(), static_cast<std::size_t>(0), "active");
}

void PaperMaker_DuplicateApprovedQuoteIgnored() {
    PaperMakerExecutionAdapter adapter(PaperMakerFillMode::Conservative);
    const auto quote = approved_quote();

    const auto first = adapter.submit_approved_quote(quote, 1'100);
    const auto duplicate = adapter.submit_approved_quote(quote, 1'200);

    expect_true(first.ok, "first");
    expect_true(duplicate.ok, "duplicate reported ok");
    expect_true(duplicate.duplicate_ignored, "duplicate ignored");
    expect_equal(adapter.quote_book().active_quote_count(), static_cast<std::size_t>(1), "active");
    expect_equal(
        adapter.quote_book().duplicate_ignored_count(),
        static_cast<std::size_t>(1),
        "duplicates"
    );
}

void PaperMaker_CancelRemovesQuote() {
    PaperMakerExecutionAdapter adapter(PaperMakerFillMode::Conservative);
    const auto quote = bid_only_quote();

    expect_true(adapter.submit_approved_quote(quote, 1'100).ok, "submit");
    expect_true(
        adapter.cancel_quote_group(quote.quote_group_id, 1'200).ok,
        "cancel"
    );
    expect_equal(adapter.quote_book().active_quote_count(), static_cast<std::size_t>(0), "active");

    const auto reports = adapter.on_market_event(conservative_trade(494'000, 10));
    expect_true(reports.empty(), "cancelled no fills");
}

void PaperMaker_ReplaceCancelsOldAndPostsNew() {
    PaperMakerExecutionAdapter adapter(PaperMakerFillMode::BookCross);
    const auto first = approved_quote(301, 401);
    auto second = approved_quote(301, 402);
    second.bid.price_tick = 496'000;
    second.approved_quote_id = compute_approved_quote_hash(second);

    const auto first_submit = adapter.submit_approved_quote(first, 1'100);
    const auto second_submit = adapter.submit_approved_quote(second, 1'200);

    expect_true(first_submit.ok, "first");
    expect_true(second_submit.ok, "second");
    expect_true(second_submit.replaced, "replaced");
    expect_equal(adapter.quote_book().replaced_quote_count(), static_cast<std::size_t>(1), "replace count");
    expect_equal(adapter.quote_book().active_quote_count(), static_cast<std::size_t>(1), "active");

    const auto depth = depth_view(490'000, 495'000);
    PaperMakerMarketEvent event;
    event.ts_ns = 2'000;
    event.asset_index = 7;
    event.depth = &depth;
    const auto reports = adapter.on_market_event(event);
    expect_equal(reports.size(), static_cast<std::size_t>(1), "report count");
    expect_equal(reports[0].approved_quote_id, second.approved_quote_id, "new approved id");
    expect_equal(reports[0].avg_fill_price_tick, 496'000LL, "new price");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"PaperMaker_EmitsMakerExecutionReport", &PaperMaker_EmitsMakerExecutionReport},
        {"PaperMaker_DuplicateApprovedQuoteIgnored", &PaperMaker_DuplicateApprovedQuoteIgnored},
        {"PaperMaker_CancelRemovesQuote", &PaperMaker_CancelRemovesQuote},
        {"PaperMaker_ReplaceCancelsOldAndPostsNew", &PaperMaker_ReplaceCancelsOldAndPostsNew},
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
