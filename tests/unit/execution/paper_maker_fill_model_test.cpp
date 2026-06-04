#include "engine/execution/adapter/PaperMakerFillModel.h"

#include "engine/state/book/DepthPrefix.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::execution::MakerQuoteStatus;
using trading_engine::execution::PaperMakerFillMode;
using trading_engine::execution::PaperMakerFillModel;
using trading_engine::execution::PaperMakerMarketEvent;
using trading_engine::execution::PaperMakerQuote;
using trading_engine::execution::QuoteSide;
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

PaperMakerQuote quote() {
    PaperMakerQuote out;
    out.quote_id = 101;
    out.approved_quote_id = 201;
    out.quote_intent_id = 301;
    out.quote_group_id = 401;
    out.asset_index = 7;
    out.asset_id = "asset-7";
    out.has_bid = true;
    out.has_ask = true;
    out.bid.asset_id = out.asset_id;
    out.bid.market_id = "market";
    out.bid.asset_index = 7;
    out.bid.market_index = 3;
    out.bid.side = QuoteSide::Bid;
    out.bid.price_tick = 495'000;
    out.bid.quantity_lots = 10;
    out.ask = out.bid;
    out.ask.side = QuoteSide::Ask;
    out.ask.price_tick = 505'000;
    out.status = MakerQuoteStatus::ActivePaper;
    out.created_ts_ns = 1'000;
    out.expires_at_ns = 5'000;
    out.idempotency_hash = 501;
    return out;
}

PaperMakerMarketEvent trade_event(std::int64_t price_tick, std::int64_t qty_lots) {
    PaperMakerMarketEvent event;
    event.ts_ns = 2'000;
    event.asset_index = 7;
    event.has_trade = true;
    event.trade_price_tick = price_tick;
    event.trade_qty_lots = qty_lots;
    return event;
}

void PaperMaker_NoFillModeDoesNotFill() {
    PaperMakerFillModel model;
    const auto fills =
        model.evaluate(quote(), trade_event(490'000, 5), PaperMakerFillMode::NoFill);
    expect_true(fills.empty(), "no fill mode empty");
}

void PaperMaker_ConservativeBidFillsOnlyWhenTradeThrough() {
    PaperMakerFillModel model;
    const auto no_fill = model.evaluate(
        quote(),
        trade_event(496'000, 5),
        PaperMakerFillMode::Conservative
    );
    expect_true(no_fill.empty(), "bid not trade-through");

    const auto fills = model.evaluate(
        quote(),
        trade_event(494'000, 5),
        PaperMakerFillMode::Conservative
    );
    expect_equal(fills.size(), static_cast<std::size_t>(1), "fill count");
    expect_equal(fills[0].side, QuoteSide::Bid, "side");
    expect_equal(fills[0].filled_qty_lots, 5LL, "qty");
    expect_equal(fills[0].fill_price_tick, 495'000LL, "maker price");
}

void PaperMaker_ConservativeAskFillsOnlyWhenTradeThrough() {
    PaperMakerFillModel model;
    const auto no_fill = model.evaluate(
        quote(),
        trade_event(504'000, 5),
        PaperMakerFillMode::Conservative
    );
    expect_true(no_fill.empty(), "ask not trade-through");

    const auto fills = model.evaluate(
        quote(),
        trade_event(506'000, 5),
        PaperMakerFillMode::Conservative
    );
    expect_equal(fills.size(), static_cast<std::size_t>(1), "fill count");
    expect_equal(fills[0].side, QuoteSide::Ask, "side");
    expect_equal(fills[0].filled_qty_lots, 5LL, "qty");
    expect_equal(fills[0].fill_price_tick, 505'000LL, "maker price");
}

void PaperMaker_ConservativeDoesNotFillWithoutTradeEvidence() {
    PaperMakerFillModel model;
    PaperMakerMarketEvent event;
    event.ts_ns = 2'000;
    event.asset_index = 7;
    event.depth = nullptr;

    const auto fills =
        model.evaluate(quote(), event, PaperMakerFillMode::Conservative);
    expect_true(fills.empty(), "no trade evidence");
}

void PaperMaker_BookCrossBidFillsWhenBidCrossesAsk() {
    PaperMakerFillModel model;
    const auto depth = depth_view(490'000, 494'000);
    PaperMakerMarketEvent event;
    event.ts_ns = 2'000;
    event.asset_index = 7;
    event.depth = &depth;

    const auto fills =
        model.evaluate(quote(), event, PaperMakerFillMode::BookCross);
    expect_equal(fills.size(), static_cast<std::size_t>(1), "fill count");
    expect_equal(fills[0].side, QuoteSide::Bid, "side");
    expect_equal(fills[0].filled_qty_lots, 10LL, "qty");
}

void PaperMaker_BookCrossAskFillsWhenAskCrossesBid() {
    PaperMakerFillModel model;
    const auto depth = depth_view(506'000, 510'000);
    PaperMakerMarketEvent event;
    event.ts_ns = 2'000;
    event.asset_index = 7;
    event.depth = &depth;

    const auto fills =
        model.evaluate(quote(), event, PaperMakerFillMode::BookCross);
    expect_equal(fills.size(), static_cast<std::size_t>(1), "fill count");
    expect_equal(fills[0].side, QuoteSide::Ask, "side");
    expect_equal(fills[0].filled_qty_lots, 10LL, "qty");
}

void PaperMaker_QueueAwareRequiresRestingTime() {
    PaperMakerFillModel model;
    auto resting_quote = quote();
    resting_quote.expires_at_ns = 1'000'000'000;
    const auto depth = depth_view(490'000, 494'000);
    PaperMakerMarketEvent event;
    event.ts_ns = resting_quote.created_ts_ns + 100'000'000;
    event.asset_index = 7;
    event.depth = &depth;

    const auto fills = model.evaluate(
        resting_quote,
        event,
        PaperMakerFillMode::QueueAware,
        true,
        0,
        250'000'000
    );
    expect_true(fills.empty(), "no fill before min rest");
}

void PaperMaker_QueueAwareFillsOnlyOnStrictCrossAfterResting() {
    PaperMakerFillModel model;
    auto resting_quote = quote();
    resting_quote.expires_at_ns = 1'000'000'000;
    const auto depth = depth_view(490'000, 494'000);
    PaperMakerMarketEvent event;
    event.ts_ns = resting_quote.created_ts_ns + 300'000'000;
    event.asset_index = 7;
    event.depth = &depth;

    const auto fills = model.evaluate(
        resting_quote,
        event,
        PaperMakerFillMode::QueueAware,
        true,
        0,
        250'000'000
    );
    expect_equal(fills.size(), static_cast<std::size_t>(1), "fill count");
    expect_equal(fills[0].side, QuoteSide::Bid, "side");
    expect_equal(fills[0].reason, std::string{"queue_aware_cross_bid"}, "reason");
}

void PaperMaker_DoesNotFillExpiredQuote() {
    PaperMakerFillModel model;
    auto expired = quote();
    expired.expires_at_ns = 2'000;
    const auto fills = model.evaluate(
        expired,
        trade_event(490'000, 5),
        PaperMakerFillMode::Conservative
    );
    expect_true(fills.empty(), "expired quote");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"PaperMaker_NoFillModeDoesNotFill", &PaperMaker_NoFillModeDoesNotFill},
        {"PaperMaker_ConservativeBidFillsOnlyWhenTradeThrough", &PaperMaker_ConservativeBidFillsOnlyWhenTradeThrough},
        {"PaperMaker_ConservativeAskFillsOnlyWhenTradeThrough", &PaperMaker_ConservativeAskFillsOnlyWhenTradeThrough},
        {"PaperMaker_ConservativeDoesNotFillWithoutTradeEvidence", &PaperMaker_ConservativeDoesNotFillWithoutTradeEvidence},
        {"PaperMaker_BookCrossBidFillsWhenBidCrossesAsk", &PaperMaker_BookCrossBidFillsWhenBidCrossesAsk},
        {"PaperMaker_BookCrossAskFillsWhenAskCrossesBid", &PaperMaker_BookCrossAskFillsWhenAskCrossesBid},
        {"PaperMaker_QueueAwareRequiresRestingTime", &PaperMaker_QueueAwareRequiresRestingTime},
        {"PaperMaker_QueueAwareFillsOnlyOnStrictCrossAfterResting", &PaperMaker_QueueAwareFillsOnlyOnStrictCrossAfterResting},
        {"PaperMaker_DoesNotFillExpiredQuote", &PaperMaker_DoesNotFillExpiredQuote},
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
