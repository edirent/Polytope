#include "engine/risk/guards/ExposureGuard.h"
#include "engine/risk/guards/InventoryGuard.h"
#include "engine/risk/ledger/ReservationBook.h"
#include "engine/risk/public/RiskDecision.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

using trading_engine::risk::CostRevalidationResult;
using trading_engine::risk::ExposureGuard;
using trading_engine::risk::InventoryGuard;
using trading_engine::risk::ReservationBook;
using trading_engine::risk::RiskDecisionType;
using trading_engine::risk::RiskPolicySnapshot;
using trading_engine::risk::make_approved_decision;
using trading_engine::signal::IntentStatus;
using trading_engine::signal::OpportunityIntent;

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

OpportunityIntent make_intent(
    std::string key,
    std::string market_id,
    std::string asset_id,
    std::int64_t cost_tick,
    std::int64_t lots
) {
    OpportunityIntent intent;
    intent.intent_id = 100;
    intent.bundle_id = 200;
    intent.status = IntentStatus::PaperOpportunity;
    intent.estimated_cost_tick = cost_tick;
    intent.bundle_qty = lots;
    intent.total_edge_tick = 100;
    intent.created_ts_ns = 1'000;
    intent.expires_at_ns = 2'000;
    intent.idempotency_key = std::move(key);
    intent.leg_count = 1;
    intent.legs[0].market_id = std::move(market_id);
    intent.legs[0].asset_id = std::move(asset_id);
    intent.legs[0].quantity_lots = lots;
    intent.legs[0].estimated_cost_tick = cost_tick;
    return intent;
}

CostRevalidationResult cost(std::int64_t total_cost_tick) {
    CostRevalidationResult out;
    out.ok = true;
    out.risk_bundle_qty = 1;
    out.risk_total_cost_tick = total_cost_tick;
    out.rejection = RiskDecisionType::Approve;
    return out;
}

ReservationBook book_with_pending(
    std::int64_t pending_cost_tick,
    std::string market_id = "market-a",
    std::string asset_id = "asset-a",
    std::int64_t lots = 1
) {
    ReservationBook book;
    const auto decision = make_approved_decision(1, 123);
    const auto result = book.try_reserve(
        make_intent(
            "pending-key",
            std::move(market_id),
            std::move(asset_id),
            pending_cost_tick,
            lots
        ),
        decision,
        1'500
    );
    expect_true(result.ok, "pending reservation");
    return book;
}

void ExposureGuard_AllowsWithinLimit() {
    auto book = book_with_pending(800);
    const auto snapshot = book.snapshot();

    RiskPolicySnapshot policy;
    policy.max_total_exposure_tick = 1'000;
    policy.max_single_market_exposure_tick = 1'000;

    const auto intent =
        make_intent("new-key", "market-a", "asset-a", 100, 1);
    const auto result =
        ExposureGuard{}.check(snapshot, intent, cost(100), policy);

    expect_true(result.pass, "pass");
    expect_equal(result.rejection, RiskDecisionType::Approve, "rejection");
    expect_equal(
        result.post_total_exposure_tick,
        900LL,
        "post total exposure"
    );
}

void ExposureGuard_RejectsTotalLimit() {
    auto book = book_with_pending(800);
    const auto snapshot = book.snapshot();

    RiskPolicySnapshot policy;
    policy.max_total_exposure_tick = 1'000;
    policy.max_single_market_exposure_tick = 2'000;

    const auto intent =
        make_intent("new-key", "market-b", "asset-b", 300, 1);
    const auto result =
        ExposureGuard{}.check(snapshot, intent, cost(300), policy);

    expect_false(result.pass, "pass");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectTotalExposureLimit,
        "rejection"
    );
    expect_equal(
        result.post_total_exposure_tick,
        1'100LL,
        "post total exposure"
    );
}

void ExposureGuard_RejectsMarketLimit() {
    auto book = book_with_pending(800, "market-a", "asset-a");
    const auto snapshot = book.snapshot();

    RiskPolicySnapshot policy;
    policy.max_total_exposure_tick = 2'000;
    policy.max_single_market_exposure_tick = 1'000;

    const auto intent =
        make_intent("new-key", "market-a", "asset-b", 300, 1);
    const auto result =
        ExposureGuard{}.check(snapshot, intent, cost(300), policy);

    expect_false(result.pass, "pass");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectSingleMarketExposureLimit,
        "rejection"
    );
    expect_equal(result.rejected_market_id, std::string{"market-a"}, "market");
    expect_equal(
        result.post_market_exposure_tick,
        1'100LL,
        "post market exposure"
    );
}

void InventoryGuard_RejectsAssetLotsLimit() {
    auto book = book_with_pending(100, "market-a", "asset-a", 90);
    const auto snapshot = book.snapshot();

    RiskPolicySnapshot policy;
    policy.max_inventory_lots_per_asset = 100;

    const auto intent =
        make_intent("new-key", "market-b", "asset-a", 20, 20);
    const auto result =
        InventoryGuard{}.check(snapshot, intent, cost(20), policy);

    expect_false(result.pass, "pass");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectInventoryLimit,
        "rejection"
    );
    expect_equal(result.rejected_asset_id, std::string{"asset-a"}, "asset");
    expect_equal(result.post_asset_lots, 110LL, "post lots");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"ExposureGuard_AllowsWithinLimit", &ExposureGuard_AllowsWithinLimit},
        {"ExposureGuard_RejectsTotalLimit", &ExposureGuard_RejectsTotalLimit},
        {"ExposureGuard_RejectsMarketLimit", &ExposureGuard_RejectsMarketLimit},
        {"InventoryGuard_RejectsAssetLotsLimit", &InventoryGuard_RejectsAssetLotsLimit}
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
