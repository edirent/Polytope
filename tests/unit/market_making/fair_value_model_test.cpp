#include "engine/strategy/market_making/fair/BarrierTouchFairModel.h"
#include "engine/execution/synthetic/SyntheticCompleteSetExecutor.h"
#include "engine/strategy/market_making/canonical/CanonicalExposureMapper.h"
#include "engine/strategy/market_making/canonical/CanonicalPriceMapper.h"
#include "engine/strategy/market_making/fair/DigitalOptionFairModel.h"
#include "engine/strategy/market_making/fair/ExternalFairRuntime.h"
#include "engine/strategy/market_making/fair/FairValueModel.h"
#include "engine/strategy/market_making/fair/FixedVolProvider.h"
#include "engine/strategy/market_making/fair/InMemorySpotOracle.h"
#include "engine/strategy/market_making/fair/TradableFairBuilder.h"
#include "engine/strategy/market_making/inventory/DynamicInventoryTargeter.h"
#include "engine/strategy/market_making/research/MarkoutAttributionEngine.h"
#include "engine/strategy/market_making/risk/PortfolioTouchRiskManager.h"
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

void BarrierTouchFairModel_UpTouchAlreadyTouchedReturnsOne() {
    const auto result = BarrierTouchFairModel{}.compute(
        BarrierTouchFairInput{
            .spot = 91.0,
            .barrier = 90.0,
            .annualized_vol = 0.90,
            .tte_years = 30.0 / 365.25,
            .event_type = ExternalFairEventType::UpTouch,
            .price_scale_tick = 10'000
        }
    );
    expect_true(result.ok, "ok");
    expect_equal(result.yes_probability, 1.0, "probability");
    expect_equal(result.fair_value_tick, 10'000LL, "fair");
}

void BarrierTouchFairModel_DownTouchAlreadyTouchedReturnsOne() {
    const auto result = BarrierTouchFairModel{}.compute(
        BarrierTouchFairInput{
            .spot = 59.0,
            .barrier = 60.0,
            .annualized_vol = 0.90,
            .tte_years = 30.0 / 365.25,
            .event_type = ExternalFairEventType::DownTouch,
            .price_scale_tick = 10'000
        }
    );
    expect_true(result.ok, "ok");
    expect_equal(result.yes_probability, 1.0, "probability");
    expect_equal(result.fair_value_tick, 10'000LL, "fair");
}

void BarrierTouchFairModel_UpTouchProbabilityIsReasonable() {
    const auto result = BarrierTouchFairModel{}.compute(
        BarrierTouchFairInput{
            .spot = 72.0,
            .barrier = 90.0,
            .annualized_vol = 0.90,
            .tte_years = 22.0 / 365.25,
            .event_type = ExternalFairEventType::UpTouch,
            .price_scale_tick = 10'000
        }
    );
    expect_true(result.ok, "ok");
    expect_true(result.yes_probability > 0.25, "probability lower");
    expect_true(result.yes_probability < 0.40, "probability upper");
}

void BarrierTouchFairModel_DownTouchProbabilityIsReasonable() {
    const auto result = BarrierTouchFairModel{}.compute(
        BarrierTouchFairInput{
            .spot = 72.0,
            .barrier = 60.0,
            .annualized_vol = 0.90,
            .tte_years = 22.0 / 365.25,
            .event_type = ExternalFairEventType::DownTouch,
            .price_scale_tick = 10'000
        }
    );
    expect_true(result.ok, "ok");
    expect_true(result.yes_probability > 0.35, "probability lower");
    expect_true(result.yes_probability < 0.50, "probability upper");
}

void BarrierTouchFairModel_FarDownTouchProbabilityLowerThanNearDownTouch() {
    BarrierTouchFairInput near_input{
        .spot = 72.0,
        .barrier = 60.0,
        .annualized_vol = 0.90,
        .tte_years = 22.0 / 365.25,
        .event_type = ExternalFairEventType::DownTouch,
        .price_scale_tick = 10'000
    };
    auto far_input = near_input;
    far_input.barrier = 50.0;

    const auto near_result = BarrierTouchFairModel{}.compute(near_input);
    const auto far_result = BarrierTouchFairModel{}.compute(far_input);
    expect_true(near_result.ok, "near ok");
    expect_true(far_result.ok, "far ok");
    expect_true(
        near_result.yes_probability > far_result.yes_probability,
        "near greater than far"
    );
}

void BarrierTouchFairModel_InvalidInputsReject() {
    const auto result = BarrierTouchFairModel{}.compute(
        BarrierTouchFairInput{
            .spot = 0.0,
            .barrier = 90.0,
            .annualized_vol = 0.90,
            .tte_years = 22.0 / 365.25,
            .event_type = ExternalFairEventType::UpTouch,
            .price_scale_tick = 10'000
        }
    );
    expect_false(result.ok, "ok");
}

ExternalFairMarketSpec make_runtime_spec(
    OutcomeSide side = OutcomeSide::Yes,
    std::int64_t now_ms = 1'000'000
) {
    ExternalFairMarketSpec spec;
    spec.market_id = "sol_june_below60";
    spec.token_id = side == OutcomeSide::Yes ? "yes" : "no";
    spec.symbol = ExternalFairSymbol::SOL;
    spec.event_type = ExternalFairEventType::DownTouch;
    spec.outcome_side = side;
    spec.barrier_price = 60.0;
    spec.expiry_unix_ms = now_ms + 22LL * 24 * 60 * 60 * 1000;
    spec.price_scale_tick = 10'000;
    return spec;
}

void ExternalFairRuntime_StaleSpotRejects() {
    constexpr std::int64_t now_ms = 1'000'000;
    InMemorySpotOracle spot_oracle;
    spot_oracle.update_sol_book_ticker(71.9, 72.1, now_ms - 2'000, now_ms - 2'000);
    FixedVolProvider vol_provider(0.90, now_ms);
    const auto result =
        ExternalFairRuntime{spot_oracle, vol_provider}.compute(
            make_runtime_spec(OutcomeSide::Yes, now_ms),
            now_ms
        );
    expect_false(result.ok, "ok");
    expect_equal(result.reject_reason, std::string{"stale_spot"}, "reason");
}

void ExternalFairRuntime_StaleVolRejects() {
    constexpr std::int64_t now_ms = 1'000'000;
    InMemorySpotOracle spot_oracle;
    spot_oracle.update_sol_book_ticker(71.9, 72.1, now_ms, now_ms);
    FixedVolProvider vol_provider(0.90, now_ms - 61'000);
    const auto result =
        ExternalFairRuntime{spot_oracle, vol_provider}.compute(
            make_runtime_spec(OutcomeSide::Yes, now_ms),
            now_ms
        );
    expect_false(result.ok, "ok");
    expect_equal(result.reject_reason, std::string{"stale_vol"}, "reason");
}

void ExternalFairRuntime_VolOutOfBoundsRejects() {
    constexpr std::int64_t now_ms = 1'000'000;
    InMemorySpotOracle spot_oracle;
    spot_oracle.update_sol_book_ticker(71.9, 72.1, now_ms, now_ms);
    FixedVolProvider vol_provider(0.05, now_ms);
    const auto result =
        ExternalFairRuntime{spot_oracle, vol_provider}.compute(
            make_runtime_spec(OutcomeSide::Yes, now_ms),
            now_ms
        );
    expect_false(result.ok, "ok");
    expect_equal(result.reject_reason, std::string{"vol_out_of_bounds"}, "reason");
}

void ExternalFairRuntime_YesTokenFairEqualsModelFair() {
    constexpr std::int64_t now_ms = 1'000'000;
    InMemorySpotOracle spot_oracle;
    spot_oracle.update_sol_book_ticker(71.9, 72.1, now_ms, now_ms);
    FixedVolProvider vol_provider(0.90, now_ms);
    const auto result =
        ExternalFairRuntime{spot_oracle, vol_provider}.compute(
            make_runtime_spec(OutcomeSide::Yes, now_ms),
            now_ms
        );
    expect_true(result.ok, "ok");
    const auto model = BarrierTouchFairModel{}.compute(
        BarrierTouchFairInput{
            .spot = 72.0,
            .barrier = 60.0,
            .annualized_vol = 0.90,
            .tte_years = 22.0 / 365.25,
            .event_type = ExternalFairEventType::DownTouch,
            .price_scale_tick = 10'000
        }
    );
    expect_true(model.ok, "model ok");
    expect_equal(result.fair_value_tick, model.fair_value_tick, "fair");
}

void ExternalFairRuntime_NoTokenMapsToOneMinusYesFair() {
    constexpr std::int64_t now_ms = 1'000'000;
    InMemorySpotOracle spot_oracle;
    spot_oracle.update_sol_book_ticker(71.9, 72.1, now_ms, now_ms);
    FixedVolProvider vol_provider(0.90, now_ms);
    ExternalFairRuntime runtime{spot_oracle, vol_provider};
    const auto yes = runtime.compute(
        make_runtime_spec(OutcomeSide::Yes, now_ms),
        now_ms
    );
    const auto no = runtime.compute(
        make_runtime_spec(OutcomeSide::No, now_ms),
        now_ms
    );
    expect_true(yes.ok, "yes ok");
    expect_true(no.ok, "no ok");
    expect_equal(no.fair_value_tick, 10'000LL - yes.fair_value_tick, "no fair");
}

void CanonicalPriceMapper_InvertsNoBook() {
    const auto yes = canonical_yes_top_of_book(
        OutcomeSide::No,
        380'000,
        390'000,
        kPriceOneTick
    );
    expect_equal(yes.bid_tick, 610'000LL, "yes bid");
    expect_equal(yes.ask_tick, 620'000LL, "yes ask");
    expect_equal(yes.mid_tick, 615'000LL, "yes mid");
    expect_equal(
        canonical_yes_to_asset_tick(OutcomeSide::No, 615'000, kPriceOneTick),
        385'000LL,
        "asset mid"
    );
}

void CanonicalExposureMapper_NoAssetFlipsSign() {
    expect_equal(
        to_canonical_yes_exposure(OutcomeSide::No, 12),
        -12LL,
        "long no"
    );
    expect_equal(
        canonical_yes_delta_for_asset_fill(OutcomeSide::No, false, 5),
        5LL,
        "sell no"
    );
}

void TradableFairBuilder_BlendsTowardExternal() {
    TradableFairBuilder builder;
    const auto out = builder.build(TradableFairInput{
        .asset_side = OutcomeSide::Yes,
        .price_scale_tick = kPriceOneTick,
        .lambda = 0.20,
        .shadow_only = false,
        .external = ExternalFairOutput{
            .ok = true,
            .canonical_yes_raw_fair_tick = 700'000,
            .asset_raw_fair_tick = 700'000,
            .confidence_bps = 10'000
        },
        .market = MarketImpliedFairOutput{
            .ok = true,
            .canonical_yes_market_mid_tick = 500'000,
            .implied_fair_tick = 500'000,
            .confidence_bps = 10'000
        }
    });
    expect_true(out.ok, "tradable ok");
    expect_equal(out.canonical_yes_tradable_fair_tick, 540'000LL, "blend");
}

void DynamicInventoryTargeter_UsesEdgeBuckets() {
    DynamicInventoryTargeter targeter;
    const auto out = targeter.compute(InventoryTargetInput{
        .event_type = ExternalFairEventType::DownTouch,
        .canonical_yes_market_mid_tick = 400'000,
        .canonical_yes_tradable_fair_tick = 435'000,
        .confidence_bps = 10'000,
        .current_canonical_yes_position_lots = 0
    });
    expect_equal(out.target_canonical_yes_lots, 15LL, "target lots");
}

void PortfolioTouchRiskManager_RejectsUpsideCap() {
    PortfolioTouchRiskManager manager;
    const auto out = manager.evaluate(PortfolioTouchRiskInput{
        .event_type = ExternalFairEventType::UpTouch,
        .current_canonical_yes_position_lots = 20,
        .proposed_canonical_yes_delta_lots = 10,
        .max_total_touch_yes_lots = 75,
        .max_upside_touch_lots = 25,
        .max_downside_touch_lots = 50
    });
    expect_false(out.ok, "risk ok");
    expect_equal(out.reason, std::string{"max_upside_touch_lots"}, "reason");
}

void SyntheticCompleteSetExecutor_PaperDetectsRichNo() {
    trading_engine::execution::synthetic::SyntheticCompleteSetExecutor exec;
    const auto out = exec.evaluate_paper(
        trading_engine::execution::synthetic::SyntheticCompleteSetInput{
            .rich_leg_side = OutcomeSide::No,
            .rich_leg_bid_tick = 850'000,
            .rich_leg_tradable_fair_tick = 820'000,
            .cheap_leg_tradable_fair_tick = 180'000,
            .residual_inventory_lots = 0,
            .target_residual_inventory_lots = 10,
            .available_cash_tick = 10'000'000,
            .min_edge_tick = 10'000,
            .min_exit_depth_lots = 1,
            .exit_depth_lots = 5,
            .tte_ns = 3'600'000'000'000LL
        }
    );
    expect_true(out.should_mint_and_sell_rich_leg, "synthetic decision");
}

void MarkoutAttributionEngine_ComputesBuyMarkout() {
    trading_engine::strategy::market_making::research::MarkoutAttributionEngine
        engine;
    const auto out = engine.attribute(
        trading_engine::strategy::market_making::research::FillAttributionInput{
            .fill_price_tick = 500'000,
            .fair_at_fill_tick = 510'000,
            .mid_at_fill_tick = 505'000,
            .future_mid_tick = 520'000,
            .future_fair_tick = 530'000,
            .buy = true
        }
    );
    expect_equal(out.markout_tick, 20'000LL, "markout");
    expect_equal(out.spread_capture_tick, 5'000LL, "spread capture");
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
        {"FairValueModel_RejectsStaleBook", &FairValueModel_RejectsStaleBook},
        {"BarrierTouchFairModel_UpTouchAlreadyTouchedReturnsOne", &BarrierTouchFairModel_UpTouchAlreadyTouchedReturnsOne},
        {"BarrierTouchFairModel_DownTouchAlreadyTouchedReturnsOne", &BarrierTouchFairModel_DownTouchAlreadyTouchedReturnsOne},
        {"BarrierTouchFairModel_UpTouchProbabilityIsReasonable", &BarrierTouchFairModel_UpTouchProbabilityIsReasonable},
        {"BarrierTouchFairModel_DownTouchProbabilityIsReasonable", &BarrierTouchFairModel_DownTouchProbabilityIsReasonable},
        {"BarrierTouchFairModel_FarDownTouchProbabilityLowerThanNearDownTouch", &BarrierTouchFairModel_FarDownTouchProbabilityLowerThanNearDownTouch},
        {"BarrierTouchFairModel_InvalidInputsReject", &BarrierTouchFairModel_InvalidInputsReject},
        {"ExternalFairRuntime_StaleSpotRejects", &ExternalFairRuntime_StaleSpotRejects},
        {"ExternalFairRuntime_StaleVolRejects", &ExternalFairRuntime_StaleVolRejects},
        {"ExternalFairRuntime_VolOutOfBoundsRejects", &ExternalFairRuntime_VolOutOfBoundsRejects},
        {"ExternalFairRuntime_YesTokenFairEqualsModelFair", &ExternalFairRuntime_YesTokenFairEqualsModelFair},
        {"ExternalFairRuntime_NoTokenMapsToOneMinusYesFair", &ExternalFairRuntime_NoTokenMapsToOneMinusYesFair},
        {"CanonicalPriceMapper_InvertsNoBook", &CanonicalPriceMapper_InvertsNoBook},
        {"CanonicalExposureMapper_NoAssetFlipsSign", &CanonicalExposureMapper_NoAssetFlipsSign},
        {"TradableFairBuilder_BlendsTowardExternal", &TradableFairBuilder_BlendsTowardExternal},
        {"DynamicInventoryTargeter_UsesEdgeBuckets", &DynamicInventoryTargeter_UsesEdgeBuckets},
        {"PortfolioTouchRiskManager_RejectsUpsideCap", &PortfolioTouchRiskManager_RejectsUpsideCap},
        {"SyntheticCompleteSetExecutor_PaperDetectsRichNo", &SyntheticCompleteSetExecutor_PaperDetectsRichNo},
        {"MarkoutAttributionEngine_ComputesBuyMarkout", &MarkoutAttributionEngine_ComputesBuyMarkout}
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
