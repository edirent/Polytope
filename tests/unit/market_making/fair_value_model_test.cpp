#include "engine/strategy/market_making/fair/FairValueModel.h"
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

void expect_false(bool value, const std::string& field) {
    if (value) {
        fail("expected false: " + field);
    }
}

void FairValueModel_MidFairValue() {
    auto depth = tools::make_depth_view(400'000, 600'000, 10.0, 10.0, 1, 100);
    const auto result = FairValueModel{}.compute(depth, MarketMakingConfig{}, 100);
    expect_equal(result.ok, true, "ok");
    expect_equal(result.fair_value_tick, 500'000LL, "fair");
    expect_equal(result.quality, FairValueQuality::Good, "quality");
}

void FairValueModel_RejectsMissingBidAsk() {
    auto depth = tools::make_depth_view(400'000, 600'000, 10.0, 10.0, 1, 100);
    depth.ask_count = 0;
    const auto result = FairValueModel{}.compute(depth, MarketMakingConfig{}, 100);
    expect_false(result.ok, "ok");
    expect_equal(result.quality, FairValueQuality::MissingBidAsk, "quality");
}

void FairValueModel_RejectsCrossedBook() {
    auto depth = tools::make_depth_view(600'000, 500'000, 10.0, 10.0, 1, 100);
    const auto result = FairValueModel{}.compute(depth, MarketMakingConfig{}, 100);
    expect_false(result.ok, "ok");
    expect_equal(result.quality, FairValueQuality::CrossedBook, "quality");
}

void FairValueModel_RejectsStaleBook() {
    auto depth = tools::make_depth_view(400'000, 600'000, 10.0, 10.0, 1, 100);
    MarketMakingConfig config;
    config.max_book_age_ns = 10;
    const auto result = FairValueModel{}.compute(depth, config, 200);
    expect_false(result.ok, "ok");
    expect_equal(result.quality, FairValueQuality::StaleBook, "quality");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> map{
        {"FairValueModel_MidFairValue", &FairValueModel_MidFairValue},
        {"FairValueModel_RejectsMissingBidAsk", &FairValueModel_RejectsMissingBidAsk},
        {"FairValueModel_RejectsCrossedBook", &FairValueModel_RejectsCrossedBook},
        {"FairValueModel_RejectsStaleBook", &FairValueModel_RejectsStaleBook}
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
