#include "engine/risk/quote/QuoteRiskEvaluator.h"

#include "engine/state/book/DepthPrefix.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::risk::ApprovedQuote;
using trading_engine::risk::QuoteRiskDecisionType;
using trading_engine::risk::QuoteRiskEvaluator;
using trading_engine::risk::QuoteRiskInput;
using trading_engine::risk::QuoteRiskPolicy;
using trading_engine::state::MarketDepthView;
using trading_engine::state::PriceLevel;
using trading_engine::strategy::market_making::QuoteIntent;
using trading_engine::strategy::market_making::QuoteIntentType;
using trading_engine::strategy::market_making::QuoteSide;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

void expect_false(bool value, const std::string& field) {
    if (value) {
        fail("expected false: " + field);
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

MarketDepthView depth() {
    MarketDepthView out;
    out.asset_index = 7;
    out.book_version = 9;
    out.snapshot_version_hash = 101;
    out.last_ws_recv_ns = 900;
    out.usable_for_depth = true;
    out.bid_count = 1;
    out.ask_count = 1;
    out.bids[0] = PriceLevel{.price_tick = 490'000, .price = 0.49, .size = 20.0};
    out.asks[0] = PriceLevel{.price_tick = 510'000, .price = 0.51, .size = 20.0};
    trading_engine::state::build_depth_prefix(
        out.bids,
        out.bid_count,
        out.asks,
        out.ask_count,
        &out.prefix
    );
    return out;
}

QuoteRiskPolicy policy() {
    QuoteRiskPolicy out;
    out.max_quote_qty_lots = 100;
    out.max_quote_notional_tick = 20'000'000;
    out.max_asset_inventory_lots = 100;
    out.min_edge_to_fair_tick = 1'000;
    out.max_quote_age_ns = 1'000'000'000;
    out.max_book_age_ns = 1'000'000'000;
    out.min_replace_interval_ns = 100;
    out.max_active_quotes_per_asset = 2;
    return out;
}

QuoteIntent quote(QuoteIntentType type = QuoteIntentType::PlaceQuote) {
    QuoteIntent out;
    out.quote_intent_id = 11;
    out.quote_group_id = 22;
    out.type = type;
    out.market_id = "m1";
    out.asset_id = "asset_yes";
    out.asset_index = 7;
    out.has_bid = true;
    out.has_ask = true;
    out.fair_value_tick = 500'000;
    out.created_ts_ns = 800;
    out.expires_at_ns = 2'000;
    out.snapshot_version_hash = 101;
    out.idempotency_hash = 333;
    out.bid.market_id = out.market_id;
    out.bid.asset_id = out.asset_id;
    out.bid.asset_index = out.asset_index;
    out.bid.side = QuoteSide::Bid;
    out.bid.price_tick = 495'000;
    out.bid.quantity_lots = 10;
    out.bid.fair_value_tick = out.fair_value_tick;
    out.bid.edge_to_fair_tick = 5'000;
    out.bid.snapshot_version_hash = out.snapshot_version_hash;
    out.ask.market_id = out.market_id;
    out.ask.asset_id = out.asset_id;
    out.ask.asset_index = out.asset_index;
    out.ask.side = QuoteSide::Ask;
    out.ask.price_tick = 505'000;
    out.ask.quantity_lots = 10;
    out.ask.fair_value_tick = out.fair_value_tick;
    out.ask.edge_to_fair_tick = 5'000;
    out.ask.snapshot_version_hash = out.snapshot_version_hash;
    return out;
}

QuoteRiskInput input(
    const QuoteIntent* q,
    const MarketDepthView* d,
    const QuoteRiskPolicy* p,
    std::uint64_t now_ns = 1'000
) {
    return QuoteRiskInput{
        .quote = q,
        .depth = d,
        .policy = p,
        .current_position_lots = 0,
        .active_quotes_for_asset = 0,
        .last_replace_ts_ns = 0,
        .now_ns = now_ns
    };
}

void expect_decision(
    const QuoteRiskInput& in,
    QuoteRiskDecisionType expected
) {
    const auto result = QuoteRiskEvaluator{}.evaluate(in);
    expect_equal(result.decision.decision, expected, "decision");
    expect_true(result.decision.decision_id != 0, "decision id");
    if (expected == QuoteRiskDecisionType::Approve) {
        expect_true(result.approved_quote.has_value(), "approved");
    } else {
        expect_false(result.approved_quote.has_value(), "approved");
    }
}

void QuoteRiskEvaluator_ApprovesSafeQuote() {
    const auto q = quote();
    const auto d = depth();
    const auto p = policy();
    expect_decision(input(&q, &d, &p), QuoteRiskDecisionType::Approve);
}

void QuoteRiskEvaluator_RejectsNullQuote() {
    const auto d = depth();
    const auto p = policy();
    expect_decision(input(nullptr, &d, &p), QuoteRiskDecisionType::RejectInvalidQuote);
}

void QuoteRiskEvaluator_RejectsExpiredQuote() {
    auto q = quote();
    const auto d = depth();
    const auto p = policy();
    q.expires_at_ns = 999;
    expect_decision(input(&q, &d, &p), QuoteRiskDecisionType::RejectExpiredQuote);
}

void QuoteRiskEvaluator_RejectsStaleBook() {
    auto q = quote();
    auto d = depth();
    const auto p = policy();
    q.expires_at_ns = 3'000'000'000;
    d.last_ws_recv_ns = 1;
    expect_decision(input(&q, &d, &p, 2'000'000'000), QuoteRiskDecisionType::RejectStaleBook);
}

void QuoteRiskEvaluator_RejectsCrossedBook() {
    const auto q = quote();
    auto d = depth();
    const auto p = policy();
    d.crossed = true;
    expect_decision(input(&q, &d, &p), QuoteRiskDecisionType::RejectCrossedBook);
}

void QuoteRiskEvaluator_RejectsBookNotUsable() {
    const auto q = quote();
    auto d = depth();
    const auto p = policy();
    d.usable_for_depth = false;
    expect_decision(input(&q, &d, &p), QuoteRiskDecisionType::RejectBookNotUsable);
}

void QuoteRiskEvaluator_RejectsInventoryLimit() {
    const auto q = quote();
    const auto d = depth();
    auto p = policy();
    p.max_asset_inventory_lots = 5;
    expect_decision(input(&q, &d, &p), QuoteRiskDecisionType::RejectInventoryLimit);
}

void QuoteRiskEvaluator_RejectsExposureLimit() {
    const auto q = quote();
    const auto d = depth();
    auto p = policy();
    p.max_quote_notional_tick = 1;
    expect_decision(input(&q, &d, &p), QuoteRiskDecisionType::RejectExposureLimit);
}

void QuoteRiskEvaluator_RejectsDuplicateQuote() {
    const auto q = quote();
    const auto d = depth();
    const auto p = policy();
    auto in = input(&q, &d, &p);
    in.active_quotes_for_asset = p.max_active_quotes_per_asset;
    expect_decision(in, QuoteRiskDecisionType::RejectDuplicateQuote);
}

void QuoteRiskEvaluator_RejectsTooFrequentReplace() {
    const auto q = quote(QuoteIntentType::ReplaceQuote);
    const auto d = depth();
    const auto p = policy();
    auto in = input(&q, &d, &p, 1'000);
    in.last_replace_ts_ns = 950;
    expect_decision(in, QuoteRiskDecisionType::RejectQuoteTooFrequent);
}

void QuoteRiskEvaluator_RejectsLowEdgeToFair() {
    auto q = quote();
    const auto d = depth();
    auto p = policy();
    p.min_edge_to_fair_tick = 10'000;
    expect_decision(input(&q, &d, &p), QuoteRiskDecisionType::RejectLowEdgeToFair);
}

void QuoteRiskEvaluator_RejectsUnsupportedSideIfConfigured() {
    const auto q = quote();
    const auto d = depth();
    auto p = policy();
    p.allow_ask_quotes = false;
    expect_decision(input(&q, &d, &p), QuoteRiskDecisionType::RejectUnsupportedSide);
}

void QuoteRiskEvaluator_KillSwitchRejectsAll() {
    const auto q = quote();
    const auto d = depth();
    auto p = policy();
    p.quote_kill_switch_enabled = true;
    expect_decision(input(&q, &d, &p), QuoteRiskDecisionType::RejectKillSwitch);
}

void ApprovedQuote_PreservesQuoteFields() {
    const auto q = quote();
    const auto d = depth();
    const auto p = policy();
    const auto result = QuoteRiskEvaluator{}.evaluate(input(&q, &d, &p));
    expect_true(result.approved_quote.has_value(), "approved");
    const auto& approved = *result.approved_quote;
    expect_equal(approved.quote_intent_id, q.quote_intent_id, "quote id");
    expect_equal(approved.quote_group_id, q.quote_group_id, "group");
    expect_equal(approved.has_bid, q.has_bid, "bid");
    expect_equal(approved.has_ask, q.has_ask, "ask");
    expect_equal(approved.bid.price_tick, q.bid.price_tick, "bid price");
    expect_equal(approved.ask.price_tick, q.ask.price_tick, "ask price");
    expect_equal(approved.expires_at_ns, q.expires_at_ns, "expiry");
}

void ApprovedQuote_IsNotOrder() {
    ApprovedQuote approved;
    approved.approved_quote_id = 10;
    approved.quote_intent_id = 11;
    expect_true(approved.approved_quote_id != 0, "approved quote id");
    expect_equal(approved.has_bid, false, "default bid");
    expect_equal(approved.has_ask, false, "default ask");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> map{
        {"QuoteRiskEvaluator_ApprovesSafeQuote", &QuoteRiskEvaluator_ApprovesSafeQuote},
        {"QuoteRiskEvaluator_RejectsNullQuote", &QuoteRiskEvaluator_RejectsNullQuote},
        {"QuoteRiskEvaluator_RejectsExpiredQuote", &QuoteRiskEvaluator_RejectsExpiredQuote},
        {"QuoteRiskEvaluator_RejectsStaleBook", &QuoteRiskEvaluator_RejectsStaleBook},
        {"QuoteRiskEvaluator_RejectsCrossedBook", &QuoteRiskEvaluator_RejectsCrossedBook},
        {"QuoteRiskEvaluator_RejectsBookNotUsable", &QuoteRiskEvaluator_RejectsBookNotUsable},
        {"QuoteRiskEvaluator_RejectsInventoryLimit", &QuoteRiskEvaluator_RejectsInventoryLimit},
        {"QuoteRiskEvaluator_RejectsExposureLimit", &QuoteRiskEvaluator_RejectsExposureLimit},
        {"QuoteRiskEvaluator_RejectsDuplicateQuote", &QuoteRiskEvaluator_RejectsDuplicateQuote},
        {"QuoteRiskEvaluator_RejectsTooFrequentReplace", &QuoteRiskEvaluator_RejectsTooFrequentReplace},
        {"QuoteRiskEvaluator_RejectsLowEdgeToFair", &QuoteRiskEvaluator_RejectsLowEdgeToFair},
        {"QuoteRiskEvaluator_RejectsUnsupportedSideIfConfigured", &QuoteRiskEvaluator_RejectsUnsupportedSideIfConfigured},
        {"QuoteRiskEvaluator_KillSwitchRejectsAll", &QuoteRiskEvaluator_KillSwitchRejectsAll},
        {"ApprovedQuote_PreservesQuoteFields", &ApprovedQuote_PreservesQuoteFields},
        {"ApprovedQuote_IsNotOrder", &ApprovedQuote_IsNotOrder}
    };
    return map;
}

int run_test(const std::string& name) {
    const auto it = tests().find(name);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << name << '\n';
        return 2;
    }
    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << name << " failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << name << " passed\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        return run_test(argv[1]);
    }
    int failures = 0;
    for (const auto& [name, _] : tests()) {
        failures += run_test(name) == 0 ? 0 : 1;
    }
    return failures == 0 ? 0 : 1;
}
