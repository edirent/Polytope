#include "engine/strategy/market_making/refresh/QuoteRefreshPolicy.h"
#include "engine/strategy/market_making/state/ActiveQuoteState.h"
#include "engine/strategy/market_making/tools/MarketMakingTools.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using namespace trading_engine::strategy::market_making;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

template <typename Actual, typename Expected>
void expect_equal(const Actual& actual, const Expected& expected, const std::string& field) {
    if (!(actual == expected)) {
        fail("mismatch: " + field);
    }
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

ActiveQuoteState active() {
    ActiveQuoteState state;
    state.asset_index = 7;
    state.has_bid = true;
    state.bid.side = QuoteSide::Bid;
    state.bid.price_tick = 495'000;
    state.bid.quantity_lots = 10;
    state.fair_value_tick = 500'000;
    state.current_position_lots = 10;
    state.created_ts_ns = 100;
    state.expires_at_ns = 1'000;
    state.idempotency_hash = 1;
    return state;
}

QuoteIntent candidate(std::int64_t fair) {
    QuoteIntent intent;
    intent.asset_index = 7;
    intent.has_bid = true;
    intent.bid.side = QuoteSide::Bid;
    intent.bid.price_tick = fair - 5'000;
    intent.bid.quantity_lots = 10;
    intent.fair_value_tick = fair;
    intent.current_position_lots = 10;
    intent.idempotency_hash = 2;
    return intent;
}

void QuoteRefreshPolicy_ReplacesOnFairMove() {
    auto depth = tools::make_depth_view(490'000, 510'000, 10.0, 10.0, 1, 100);
    MarketMakingConfig config;
    config.requote_threshold_tick = 5'000;
    const auto current = active();
    const auto next = candidate(510'000);
    const auto decision = QuoteRefreshPolicy{}.evaluate(
        &current,
        &next,
        depth,
        config,
        10,
        200
    );
    expect_true(decision.should_cancel, "cancel");
    expect_true(decision.should_replace, "replace");
    expect_equal(decision.reason, CancelReason::FairValueMoved, "reason");
}

void QuoteRefreshPolicy_DoesNotReplaceSameShapeWithNewHash() {
    auto depth = tools::make_depth_view(490'000, 510'000, 10.0, 10.0, 1, 100);
    MarketMakingConfig config;
    config.requote_threshold_tick = 5'000;
    const auto current = active();
    auto next = candidate(500'000);
    next.idempotency_hash = 999;

    const auto decision = QuoteRefreshPolicy{}.evaluate(
        &current,
        &next,
        depth,
        config,
        10,
        200
    );
    expect_true(!decision.should_cancel, "no cancel");
    expect_true(!decision.should_replace, "no replace");
}

void QuoteRefreshPolicy_HoldsChangedQuoteInsideTwoMsCooldown() {
    auto depth = tools::make_depth_view(490'000, 510'000, 10.0, 10.0, 1, 100);
    MarketMakingConfig config;
    config.requote_threshold_tick = 1;
    config.min_requote_interval_ns = 2'000'000;
    auto current = active();
    current.expires_at_ns = 10'000'000;
    auto next = candidate(510'000);

    const auto decision = QuoteRefreshPolicy{}.evaluate(
        &current,
        &next,
        depth,
        config,
        10,
        current.created_ts_ns + 1'000'000
    );
    expect_true(!decision.should_cancel, "no cancel");
    expect_true(!decision.should_replace, "no replace");
}

void QuoteRefreshPolicy_CancelsOnStaleBook() {
    auto depth = tools::make_depth_view(490'000, 510'000, 10.0, 10.0, 1, 100);
    depth.usable_for_depth = false;
    const auto current = active();
    const auto next = candidate(500'000);
    const auto decision = QuoteRefreshPolicy{}.evaluate(
        &current,
        &next,
        depth,
        MarketMakingConfig{},
        10,
        200
    );
    expect_true(decision.should_cancel, "cancel");
    expect_equal(decision.reason, CancelReason::BookStale, "reason");
}

void QuoteRefreshPolicy_CancelsOnTtl() {
    auto depth = tools::make_depth_view(490'000, 510'000, 10.0, 10.0, 1, 100);
    const auto current = active();
    const auto next = candidate(500'000);
    const auto decision = QuoteRefreshPolicy{}.evaluate(
        &current,
        &next,
        depth,
        MarketMakingConfig{},
        10,
        1'000
    );
    expect_true(decision.should_cancel, "cancel");
    expect_true(decision.should_replace, "replace");
    expect_equal(decision.reason, CancelReason::QuoteExpired, "reason");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> map{
        {"QuoteRefreshPolicy_ReplacesOnFairMove", &QuoteRefreshPolicy_ReplacesOnFairMove},
        {"QuoteRefreshPolicy_DoesNotReplaceSameShapeWithNewHash", &QuoteRefreshPolicy_DoesNotReplaceSameShapeWithNewHash},
        {"QuoteRefreshPolicy_HoldsChangedQuoteInsideTwoMsCooldown", &QuoteRefreshPolicy_HoldsChangedQuoteInsideTwoMsCooldown},
        {"QuoteRefreshPolicy_CancelsOnStaleBook", &QuoteRefreshPolicy_CancelsOnStaleBook},
        {"QuoteRefreshPolicy_CancelsOnTtl", &QuoteRefreshPolicy_CancelsOnTtl}
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
