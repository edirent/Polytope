#include "engine/strategy/market_making/public/CancelQuoteIntent.h"
#include "engine/strategy/market_making/public/MarketMakingConfig.h"
#include "engine/strategy/market_making/public/QuoteIntent.h"

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

void QuoteIntent_DefaultNone() {
    const QuoteIntent intent;
    expect_equal(intent.type, QuoteIntentType::None, "type");
    expect_equal(intent.has_bid, false, "has_bid");
    expect_equal(intent.has_ask, false, "has_ask");
}

void QuoteIntent_HasHashes() {
    QuoteIntent intent;
    intent.quote_intent_id = 1;
    intent.type = QuoteIntentType::PlaceQuote;
    intent.quote_group_id = 2;
    intent.asset_index = 3;
    intent.fair_value_tick = 500'000;
    intent.idempotency_hash = 4;
    expect_true(compute_quote_intent_hash(intent) != 0, "hash");
}

void CancelQuoteIntent_HasReason() {
    CancelQuoteIntent cancel;
    cancel.reason = CancelReason::QuoteExpired;
    cancel.active_quote_id = 7;
    expect_equal(cancel.reason, CancelReason::QuoteExpired, "reason");
    expect_true(compute_cancel_quote_intent_hash(cancel) != 0, "hash");
}

void MarketMakingConfig_DefaultsSafe() {
    const MarketMakingConfig config;
    expect_true(config.base_quote_size_lots > 0, "base size");
    expect_true(config.max_inventory_lots > config.min_inventory_lots, "inventory range");
    expect_true(config.min_price_tick < config.max_price_tick, "price range");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> map{
        {"QuoteIntent_DefaultNone", &QuoteIntent_DefaultNone},
        {"QuoteIntent_HasHashes", &QuoteIntent_HasHashes},
        {"CancelQuoteIntent_HasReason", &CancelQuoteIntent_HasReason},
        {"MarketMakingConfig_DefaultsSafe", &MarketMakingConfig_DefaultsSafe}
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
