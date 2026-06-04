#include "engine/strategy/market_making/fair/DigitalOptionFairModel.h"
#include "engine/strategy/market_making/fair/FairValueModel.h"
#include "engine/strategy/market_making/tools/MarketMakingTools.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using namespace trading_engine::strategy::market_making;

constexpr std::uint64_t kNsPerSecond = 1'000'000'000ULL;

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

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

void DigitalOptionFairModel_PricesAtmDigitalBelowHalf() {
    const auto result = DigitalOptionFairModel{}.compute(
        DigitalOptionFairInput{
            .spot = 70'000.0,
            .strike = 70'000.0,
            .vol_annual_bps = 5'000.0,
            .time_to_expiry_ns = 30 * 24 * 60 * 60 * kNsPerSecond
        }
    );
    expect_true(result.ok, "ok");
    expect_true(result.fair_value_tick > 450'000, "fair lower bound");
    expect_true(result.fair_value_tick < 500'000, "fair below half");
    expect_true(result.probability_variance_to_expiry > 0.0, "variance");
}

void DigitalOptionFairModel_ExpiresToIntrinsic() {
    const auto in_the_money = DigitalOptionFairModel{}.compute(
        DigitalOptionFairInput{
            .spot = 70'001.0,
            .strike = 70'000.0,
            .vol_annual_bps = 5'000.0,
            .time_to_expiry_ns = 0
        }
    );
    expect_true(in_the_money.ok, "itm ok");
    expect_equal(in_the_money.fair_value_tick, 999'999LL, "itm fair");

    const auto out_of_the_money = DigitalOptionFairModel{}.compute(
        DigitalOptionFairInput{
            .spot = 69'999.0,
            .strike = 70'000.0,
            .vol_annual_bps = 5'000.0,
            .time_to_expiry_ns = 0
        }
    );
    expect_true(out_of_the_money.ok, "otm ok");
    expect_equal(out_of_the_money.fair_value_tick, 1LL, "otm fair");
}

void AvellanedaStoikovModel_WidensOnVolatilityMultiplier() {
    const auto fair = DigitalOptionFairModel{}.compute(
        DigitalOptionFairInput{
            .spot = 70'000.0,
            .strike = 70'000.0,
            .vol_annual_bps = 5'000.0,
            .time_to_expiry_ns = 30 * 24 * 60 * 60 * kNsPerSecond
        }
    );
    const auto normal = AvellanedaStoikovModel{}.compute(
        AvellanedaStoikovInput{
            .fair = fair,
            .risk_aversion = 0.05,
            .order_arrival_k = 0.02,
            .spread_multiplier = 1.0,
            .min_half_spread_tick = 1'000,
            .max_half_spread_tick = 100'000
        }
    );
    const auto toxic = AvellanedaStoikovModel{}.compute(
        AvellanedaStoikovInput{
            .fair = fair,
            .risk_aversion = 0.05,
            .order_arrival_k = 0.02,
            .spread_multiplier = 3.0,
            .min_half_spread_tick = 1'000,
            .max_half_spread_tick = 100'000
        }
    );
    expect_true(normal.ok, "normal ok");
    expect_true(toxic.ok, "toxic ok");
    expect_true(toxic.half_spread_tick > normal.half_spread_tick, "wider");
    expect_true(normal.max_inventory_skew_tick > 0, "skew");
}

void FairValueModel_MidFairValue() {
    auto depth = tools::make_depth_view(400'000, 600'000, 10.0, 10.0, 1, 100);
    const auto result = FairValueModel{}.compute(depth, MarketMakingConfig{}, 100);
    expect_equal(result.ok, true, "ok");
    expect_equal(result.fair_value_tick, 500'000LL, "fair");
    expect_equal(result.quality, FairValueQuality::Valid, "quality");
}

void FairValueModel_MicropriceMovesTowardThinAsk() {
    auto depth = tools::make_depth_view(400'000, 600'000, 30.0, 10.0, 1, 100);
    auto cfg = MarketMakingConfig{};
    cfg.min_fair_confidence_bps = 0;
    const auto result = FairValueModel{}.compute(depth, cfg, 100);
    expect_equal(result.ok, true, "ok");
    expect_equal(result.midpoint_tick, 500'000LL, "midpoint");
    expect_equal(result.microprice_tick, 550'000LL, "microprice");
    expect_equal(result.fair_value_tick, 520'000LL, "fair");
    expect_equal(result.source, FairValueSourceKind::MicropriceVwap, "source");
}

void FairValueModel_BlendsComplementImpliedFair() {
    auto up = tools::make_depth_view(400'000, 420'000, 10.0, 10.0, 1, 100);
    auto down = tools::make_depth_view(690'000, 710'000, 10.0, 10.0, 2, 100);
    auto cfg = MarketMakingConfig{};
    cfg.min_fair_confidence_bps = 0;
    cfg.complement_fair_weight_bps = 5'000;
    const auto result = FairValueModel{}.compute(up, cfg, 100, &down);
    expect_equal(result.ok, true, "ok");
    expect_equal(result.midpoint_tick, 410'000LL, "up midpoint");
    expect_equal(result.complement_midpoint_tick, 700'000LL, "down midpoint");
    expect_equal(result.complement_implied_tick, 300'000LL, "implied up");
    expect_equal(result.fair_value_tick, 355'000LL, "combined fair");
    expect_equal(result.source, FairValueSourceKind::ComplementMarket, "source");
}

void FairValueModel_ExternalFairOverridesBookFair() {
    auto depth = tools::make_depth_view(400'000, 420'000, 10.0, 10.0, 1, 100);
    auto cfg = MarketMakingConfig{};
    cfg.min_fair_confidence_bps = 0;
    cfg.external_fair_value_tick = 600'000;
    cfg.external_fair_weight_bps = 5'000;
    const auto result = FairValueModel{}.compute(depth, cfg, 100);
    expect_equal(result.ok, true, "ok");
    expect_equal(result.midpoint_tick, 410'000LL, "book fair");
    expect_equal(result.external_fair_value_tick, 600'000LL, "external");
    expect_equal(result.fair_value_tick, 600'000LL, "external fair");
    expect_equal(result.source, FairValueSourceKind::External, "source");
}

void FairValueModel_ExternalFairBypassesBookLowConfidence() {
    auto depth = tools::make_depth_view(100'000, 900'000, 1.0, 1.0, 1, 100);
    auto cfg = MarketMakingConfig{};
    cfg.max_book_spread_tick = 50'000;
    cfg.min_top_depth_lots = 10;
    cfg.min_fair_confidence_bps = 9'000;
    cfg.external_fair_value_tick = 620'000;
    cfg.external_fair_weight_bps = 10'000;
    const auto result = FairValueModel{}.compute(depth, cfg, 100);
    expect_true(result.ok, "ok");
    expect_equal(result.quality, FairValueQuality::Valid, "quality");
    expect_equal(result.fair_value_tick, 620'000LL, "external fair");
    expect_equal(result.source, FairValueSourceKind::External, "source");
}

void FairValueModel_RejectsMissingRequiredExternalFair() {
    auto depth = tools::make_depth_view(400'000, 420'000, 10.0, 10.0, 1, 100);
    auto cfg = MarketMakingConfig{};
    cfg.external_fair_weight_bps = 10'000;
    cfg.require_external_fair_for_opening_quotes = true;
    const auto result = FairValueModel{}.compute(depth, cfg, 100);
    expect_false(result.ok, "ok");
    expect_equal(result.quality, FairValueQuality::ExternalStale, "quality");
}

void FairValueModel_RejectsWideSpread() {
    auto depth = tools::make_depth_view(400'000, 600'000, 10.0, 10.0, 1, 100);
    auto cfg = MarketMakingConfig{};
    cfg.max_book_spread_tick = 50'000;
    const auto result = FairValueModel{}.compute(depth, cfg, 100);
    expect_false(result.ok, "ok");
    expect_equal(result.quality, FairValueQuality::LowConfidence, "quality");
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
        {"DigitalOptionFairModel_PricesAtmDigitalBelowHalf", &DigitalOptionFairModel_PricesAtmDigitalBelowHalf},
        {"DigitalOptionFairModel_ExpiresToIntrinsic", &DigitalOptionFairModel_ExpiresToIntrinsic},
        {"AvellanedaStoikovModel_WidensOnVolatilityMultiplier", &AvellanedaStoikovModel_WidensOnVolatilityMultiplier},
        {"FairValueModel_MidFairValue", &FairValueModel_MidFairValue},
        {"FairValueModel_MicropriceMovesTowardThinAsk", &FairValueModel_MicropriceMovesTowardThinAsk},
        {"FairValueModel_BlendsComplementImpliedFair", &FairValueModel_BlendsComplementImpliedFair},
        {"FairValueModel_ExternalFairOverridesBookFair", &FairValueModel_ExternalFairOverridesBookFair},
        {"FairValueModel_ExternalFairBypassesBookLowConfidence", &FairValueModel_ExternalFairBypassesBookLowConfidence},
        {"FairValueModel_RejectsMissingRequiredExternalFair", &FairValueModel_RejectsMissingRequiredExternalFair},
        {"FairValueModel_RejectsWideSpread", &FairValueModel_RejectsWideSpread},
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
