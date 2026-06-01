#include "engine/order_decision/limits/LimitPriceBuilder.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::order_decision::LimitPriceBuilder;
using trading_engine::order_decision::OrderDecisionType;

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

void LimitPriceBuilder_UsesWorstConsumedPrice() {
    const auto result = LimitPriceBuilder{}.build_buy_limit({
        .worst_consumed_price_tick = 500'000
    });
    expect_true(result.ok, "ok");
    expect_equal(result.limit_price_tick, 500'000LL, "limit");
}

void LimitPriceBuilder_AddsProtectionBuffer() {
    const auto result = LimitPriceBuilder{}.build_buy_limit({
        .worst_consumed_price_tick = 500'000,
        .protection_buffer_tick = 10'000
    });
    expect_true(result.ok, "ok");
    expect_equal(result.limit_price_tick, 510'000LL, "limit");
}

void LimitPriceBuilder_RejectsAboveMaxAllowed() {
    const auto result = LimitPriceBuilder{}.build_buy_limit({
        .worst_consumed_price_tick = 500'000,
        .protection_buffer_tick = 10'000,
        .configured_max_price_tick = 505'000
    });
    expect_false(result.ok, "ok");
    expect_equal(
        result.reject_reason,
        OrderDecisionType::RejectPriceProtection,
        "reason"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "LimitPriceBuilder_UsesWorstConsumedPrice",
            &LimitPriceBuilder_UsesWorstConsumedPrice
        },
        {
            "LimitPriceBuilder_AddsProtectionBuffer",
            &LimitPriceBuilder_AddsProtectionBuffer
        },
        {
            "LimitPriceBuilder_RejectsAboveMaxAllowed",
            &LimitPriceBuilder_RejectsAboveMaxAllowed
        }
    };
    return test_map;
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
