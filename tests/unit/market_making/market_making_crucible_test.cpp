#include "engine/execution/adapter/LiveOrderBridge.h"
#include "engine/execution/adapter/PaperMakerExecutionAdapter.h"
#include "engine/risk/quote/QuoteRiskEvaluator.h"
#include "engine/state/book/DepthPrefix.h"
#include "engine/strategy/market_making/core/MarketMakingEngine.h"
#include "engine/strategy/market_making/quote/QuotePriceClamp.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

namespace execution = trading_engine::execution;
namespace mm = trading_engine::strategy::market_making;
namespace risk = trading_engine::risk;
namespace state = trading_engine::state;

constexpr std::uint64_t kMs = 1'000'000ULL;
constexpr std::int64_t kFairCrashTick = 100'000;

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

state::MarketDepthView depth_view(
    std::int64_t bid_tick,
    std::int64_t ask_tick,
    double bid_size = 100.0,
    double ask_size = 100.0,
    std::uint64_t version = 1,
    std::uint64_t now_ns = 1'000'000'000ULL
) {
    state::MarketDepthView depth;
    depth.asset_index = 7;
    depth.book_version = version;
    depth.snapshot_version_hash = version * 101;
    depth.last_ws_recv_ns = now_ns;
    depth.usable_for_depth = true;
    depth.crossed = bid_tick > 0 && ask_tick > 0 && bid_tick >= ask_tick;
    depth.bid_count = 1;
    depth.ask_count = 1;
    depth.bids[0] = state::PriceLevel{
        .price_tick = bid_tick,
        .price = static_cast<double>(bid_tick) / 1'000'000.0,
        .size = bid_size
    };
    depth.asks[0] = state::PriceLevel{
        .price_tick = ask_tick,
        .price = static_cast<double>(ask_tick) / 1'000'000.0,
        .size = ask_size
    };
    state::build_depth_prefix(
        depth.bids,
        depth.bid_count,
        depth.asks,
        depth.ask_count,
        &depth.prefix
    );
    return depth;
}

mm::MarketMakingConfig defensive_config(std::int64_t max_inventory = 100) {
    mm::MarketMakingConfig config;
    config.strategy_id = 17;
    config.oracle_artifact_hash = 11;
    config.policy_hash = 22;
    config.max_inventory_lots = max_inventory;
    config.quote_ttl_ns = 5'000'000'000ULL;
    config.requote_threshold_tick = 1'000;
    config.max_book_age_ns = 1'000'000'000LL;
    return config;
}

mm::MarketMakingInput input_for(
    const state::MarketDepthView& depth,
    std::int64_t position_lots,
    std::uint64_t now_ns
) {
    return mm::MarketMakingInput{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .market_index = 1,
        .asset_index = depth.asset_index,
        .depth = &depth,
        .current_position_lots = position_lots,
        .now_ns = now_ns
    };
}

risk::QuoteRiskPolicy quote_policy(std::int64_t max_inventory = 100) {
    risk::QuoteRiskPolicy policy;
    policy.max_quote_notional_tick = 10'000'000;
    policy.max_asset_inventory_lots = max_inventory;
    policy.min_edge_to_fair_tick = -50'000;
    policy.max_book_age_ns = 1'000'000'000ULL;
    return policy;
}

risk::ApprovedQuote approve_quote(
    const mm::QuoteIntent& quote,
    const state::MarketDepthView& depth,
    std::int64_t current_position_lots,
    std::int64_t max_inventory = 100,
    std::uint64_t now_ns = 1'000'000'000ULL
) {
    const auto policy = quote_policy(max_inventory);
    const auto result = risk::QuoteRiskEvaluator{}.evaluate(risk::QuoteRiskInput{
        .quote = &quote,
        .depth = &depth,
        .policy = &policy,
        .current_position_lots = current_position_lots,
        .now_ns = now_ns
    });
    if (result.decision.decision != risk::QuoteRiskDecisionType::Approve ||
        !result.approved_quote) {
        fail("quote was not approved: " + result.decision.reason);
    }
    return *result.approved_quote;
}

void FuzzerRejectsCrossedMissingAndNegativeBooks() {
    const auto config = defensive_config();

    const std::vector<state::MarketDepthView> bad_books{
        depth_view(55, 50, 100.0, 100.0, 1, 100),
        depth_view(0, 90, 100.0, 100.0, 2, 100),
        depth_view(-1, 50, 100.0, 100.0, 3, 100),
        depth_view(50, 0, 100.0, 100.0, 4, 100),
        depth_view(50, -1, 100.0, 100.0, 5, 100)
    };

    for (const auto& bad_depth : bad_books) {
        mm::MarketMakingEngine engine(config);
        const auto result = engine.on_market_update(input_for(
            bad_depth,
            0,
            bad_depth.last_ws_recv_ns
        ));
        expect_equal(result.quote_count, static_cast<std::uint16_t>(0), "quote_count");
        expect_true(result.rejected_no_quote > 0, "rejected_no_quote");
    }

    const auto clamp = mm::clamp_quote_prices(-1, 0, config);
    expect_false(clamp.ok, "negative/zero clamp ok");
    expect_true(clamp.bid_tick > 0, "clamped bid positive");
    expect_true(clamp.ask_tick > 0, "clamped ask positive");
}

void JitterArenaToxicTakerBeatsCancelAndRealizesLoss() {
    const auto config = defensive_config();
    mm::MarketMakingEngine engine(config);
    const auto initial_depth = depth_view(
        490'000,
        510'000,
        100.0,
        100.0,
        1,
        1'000'000'000ULL
    );

    const auto quote_result =
        engine.on_market_update(input_for(initial_depth, 0, initial_depth.last_ws_recv_ns));
    expect_equal(quote_result.quote_count, static_cast<std::uint16_t>(1), "quote");
    const auto approved =
        approve_quote(quote_result.quotes[0], initial_depth, 0, 100, initial_depth.last_ws_recv_ns);
    expect_true(approved.has_bid, "approved bid");

    const auto crash_depth = depth_view(
        90'000,
        110'000,
        100.0,
        100.0,
        2,
        initial_depth.last_ws_recv_ns + 10 * kMs
    );
    const auto cancel_result =
        engine.on_market_update(input_for(crash_depth, 0, crash_depth.last_ws_recv_ns));
    expect_equal(cancel_result.cancel_count, static_cast<std::uint16_t>(1), "cancel");

    struct Action {
        std::uint64_t arrival_ns = 0;
        std::uint8_t kind = 0;
    };
    constexpr std::uint8_t kSubmit = 1;
    constexpr std::uint8_t kToxicTrade = 2;
    constexpr std::uint8_t kCancel = 3;
    std::vector<Action> actions{
        {.arrival_ns = initial_depth.last_ws_recv_ns + 45 * kMs, .kind = kSubmit},
        {.arrival_ns = initial_depth.last_ws_recv_ns + 50 * kMs, .kind = kToxicTrade},
        {.arrival_ns = initial_depth.last_ws_recv_ns + 55 * kMs, .kind = kCancel}
    };
    std::sort(actions.begin(), actions.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.arrival_ns < rhs.arrival_ns;
    });

    execution::PaperMakerExecutionAdapter venue{
        execution::PaperMakerFillMode::Conservative
    };
    std::int64_t toxic_loss_tick = 0;
    std::uint64_t fill_arrival_ns = 0;
    std::uint64_t cancel_arrival_ns = 0;
    for (const auto& action : actions) {
        if (action.kind == kSubmit) {
            const auto submit = venue.submit_approved_quote(approved, action.arrival_ns);
            expect_true(submit.ok, "submit arrives");
        } else if (action.kind == kToxicTrade) {
            execution::PaperMakerMarketEvent event;
            event.ts_ns = action.arrival_ns;
            event.asset_index = approved.bid.asset_index;
            event.has_trade = true;
            event.trade_price_tick = kFairCrashTick;
            event.trade_qty_lots = approved.bid.quantity_lots;
            event.trade_aggressor_side = execution::OrderSide::Sell;
            const auto reports = venue.on_market_event(event);
            expect_equal(reports.size(), static_cast<std::size_t>(1), "toxic fill");
            const auto& report = reports[0];
            fill_arrival_ns = action.arrival_ns;
            toxic_loss_tick =
                (report.avg_fill_price_tick - kFairCrashTick) *
                report.filled_qty_lots;
        } else if (action.kind == kCancel) {
            cancel_arrival_ns = action.arrival_ns;
            const auto cancel =
                venue.cancel_quote_group(approved.quote_group_id, action.arrival_ns);
            expect_false(cancel.ok, "cancel after toxic fill");
        }
    }

    expect_true(fill_arrival_ns != 0, "filled");
    expect_true(cancel_arrival_ns > fill_arrival_ns, "toxic before cancel");
    expect_true(toxic_loss_tick > 0, "toxic loss");
    const auto expected_loss =
        (approved.bid.price_tick - kFairCrashTick) * approved.bid.quantity_lots;
    expect_equal(toxic_loss_tick, expected_loss, "toxic loss amount");
}

void AvalancheSkewsQuotesAndRiskCutsBuyChannel() {
    constexpr std::int64_t kMaxExposure = 5'000;
    auto config = defensive_config(kMaxExposure);
    config.max_inventory_skew_tick = 75'000;

    const auto depth = depth_view(
        490'000,
        510'000,
        100.0,
        100.0,
        1,
        1'000'000'000ULL
    );
    mm::MarketMakingEngine engine(config);
    const auto near_limit =
        engine.on_market_update(input_for(depth, kMaxExposure - 1, depth.last_ws_recv_ns));
    expect_equal(near_limit.quote_count, static_cast<std::uint16_t>(1), "near limit quote");
    const auto& quote = near_limit.quotes[0];
    expect_true(quote.inventory_skew_tick >= 74'000, "large inventory skew");
    expect_true(quote.has_ask, "ask retained for inventory reduction");
    expect_true(quote.ask.price_tick < quote.fair_value_tick, "reducing ask below fair");
    if (quote.has_bid) {
        expect_true(
            quote.bid.price_tick <= quote.fair_value_tick - 90'000,
            "bid heavily biased down"
        );
    }

    mm::MarketMakingEngine saturated(config);
    const auto at_limit =
        saturated.on_market_update(input_for(depth, kMaxExposure, depth.last_ws_recv_ns));
    expect_equal(at_limit.quote_count, static_cast<std::uint16_t>(1), "at limit quote");
    expect_false(at_limit.quotes[0].has_bid, "bid channel cut at max exposure");
    expect_true(at_limit.quotes[0].has_ask, "ask still available at max exposure");

    const auto policy = quote_policy(kMaxExposure);
    const auto reducing = risk::QuoteRiskEvaluator{}.evaluate(risk::QuoteRiskInput{
        .quote = &quote,
        .depth = &depth,
        .policy = &policy,
        .current_position_lots = kMaxExposure,
        .now_ns = depth.last_ws_recv_ns
    });
    expect_equal(
        reducing.decision.decision,
        risk::QuoteRiskDecisionType::Approve,
        "risk reducing quote approved"
    );

    auto buy_quote = quote;
    buy_quote.type = mm::QuoteIntentType::PlaceQuote;
    buy_quote.has_bid = true;
    buy_quote.has_ask = false;
    buy_quote.bid = quote.ask;
    buy_quote.bid.side = mm::QuoteSide::Bid;
    buy_quote.bid.price_tick = quote.fair_value_tick - 1;
    buy_quote.bid.quantity_lots = 10;
    buy_quote.bid.edge_to_fair_tick = 1;
    const auto breach = risk::QuoteRiskEvaluator{}.evaluate(risk::QuoteRiskInput{
        .quote = &buy_quote,
        .depth = &depth,
        .policy = &policy,
        .current_position_lots = kMaxExposure,
        .now_ns = depth.last_ws_recv_ns
    });
    expect_equal(
        breach.decision.decision,
        risk::QuoteRiskDecisionType::RejectInventoryLimit,
        "inventory breach"
    );
    expect_equal(
        risk::quote_risk_rejection_reason_code(breach.decision.decision),
        risk::kQuoteRiskExposureBreachReasonCode,
        "EXPOSURE_BREACH reason code"
    );
}

class DeterministicSigner final : public execution::ILiveOrderSigner {
public:
    [[nodiscard]] execution::LiveOrderSignResult sign_order(
        const execution::LiveOrderRequest& request
    ) override {
        std::uint64_t hash = 14695981039346656037ULL;
        const auto mix = [&hash](std::uint64_t value) {
            for (int shift = 0; shift < 64; shift += 8) {
                hash ^= (value >> shift) & 0xffU;
                hash *= 1099511628211ULL;
            }
        };
        mix(request.parent_id);
        mix(request.child_id);
        mix(static_cast<std::uint64_t>(request.price_tick));
        mix(static_cast<std::uint64_t>(request.quantity_lots));
        return {
            .ok = true,
            .order = execution::SignedLiveOrder{
                .request_body_json =
                    std::string{"{\"signature\":\""} + std::to_string(hash) + "\"}",
                .venue_order_id_hint = std::to_string(hash)
            }
        };
    }
};

class ProfilingSigner final : public execution::ILiveOrderSigner {
public:
    explicit ProfilingSigner(execution::ILiveOrderSigner* inner)
        : inner_(inner) {
        samples_ns.reserve(10'000);
    }

    [[nodiscard]] execution::LiveOrderSignResult sign_order(
        const execution::LiveOrderRequest& request
    ) override {
        const auto start = Clock::now();
        auto result = inner_->sign_order(request);
        const auto end = Clock::now();
        samples_ns.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
                .count()
        ));
        return result;
    }

    std::vector<std::uint64_t> samples_ns;

private:
    using Clock = std::chrono::high_resolution_clock;

    execution::ILiveOrderSigner* inner_ = nullptr;
};

std::uint64_t percentile(std::vector<std::uint64_t> values, double pct) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(
        ((values.size() - 1) * pct) / 100.0
    );
    return values[index];
}

void SignatureLatencyP99Under100us() {
    DeterministicSigner signer;
    ProfilingSigner profiler(&signer);
    execution::LiveOrderRequest request;
    request.parent_id = 1;
    request.market_id = "m1";
    request.asset_id = "asset_yes";
    request.side = execution::OrderSide::Buy;
    request.quantity_lots = 9;
    request.price_tick = 475'000;
    request.order_type = "GTC";
    request.post_only = true;

    for (std::uint64_t i = 0; i < 10'000; ++i) {
        request.child_id = i + 1;
        const auto result = profiler.sign_order(request);
        expect_true(result.ok, "sign ok");
    }

    expect_equal(profiler.samples_ns.size(), static_cast<std::size_t>(10'000), "samples");
    const auto p99_ns = percentile(profiler.samples_ns, 99.0);
    if (p99_ns > 100'000ULL) {
        fail(
            "signature p99 exceeded 100us; move signer off the strategy thread"
        );
    }
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> map{
        {
            "FuzzerRejectsCrossedMissingAndNegativeBooks",
            &FuzzerRejectsCrossedMissingAndNegativeBooks
        },
        {
            "JitterArenaToxicTakerBeatsCancelAndRealizesLoss",
            &JitterArenaToxicTakerBeatsCancelAndRealizesLoss
        },
        {
            "AvalancheSkewsQuotesAndRiskCutsBuyChannel",
            &AvalancheSkewsQuotesAndRiskCutsBuyChannel
        },
        {"SignatureLatencyP99Under100us", &SignatureLatencyP99Under100us}
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
