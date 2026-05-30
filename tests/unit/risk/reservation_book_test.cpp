#include "engine/risk/ledger/ReservationBook.h"
#include "engine/risk/public/RiskDecision.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::risk::ReservationBook;
using trading_engine::risk::RiskRejectReason;
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

OpportunityIntent make_intent(std::string key = "idem-1") {
    OpportunityIntent intent;
    intent.intent_id = 100;
    intent.bundle_id = 200;
    intent.status = IntentStatus::PaperOpportunity;
    intent.estimated_cost_tick = 1'200;
    intent.bundle_qty = 10;
    intent.total_edge_tick = 500;
    intent.created_ts_ns = 1'000;
    intent.expires_at_ns = 2'000;
    intent.idempotency_key = std::move(key);
    intent.leg_count = 2;

    intent.legs[0].market_id = "market-a";
    intent.legs[0].asset_id = "asset-a";
    intent.legs[0].quantity_lots = 7;
    intent.legs[0].estimated_cost_tick = 700;

    intent.legs[1].market_id = "market-b";
    intent.legs[1].asset_id = "asset-b";
    intent.legs[1].quantity_lots = 3;
    intent.legs[1].estimated_cost_tick = 500;

    return intent;
}

void ReservationBook_ApprovedDecisionReserves() {
    ReservationBook book;
    const auto decision = make_approved_decision(1, 123);
    const auto result = book.try_reserve(make_intent(), decision, 1'500);

    expect_true(result.ok, "reserve ok");
    expect_true(result.reservation_id != 0, "reservation id");

    const auto snapshot = book.snapshot();
    expect_equal(snapshot.active_reservations, 1ULL, "active reservations");
    expect_equal(
        snapshot.total_reserved_cost_tick,
        1'200LL,
        "reserved cost"
    );
    expect_equal(
        snapshot.total_reserved_exposure_tick,
        1'200LL,
        "reserved exposure"
    );
    expect_equal(
        snapshot.reserved_asset_lots.at("asset-a"),
        7LL,
        "asset-a lots"
    );
    expect_equal(
        snapshot.reserved_market_exposure_tick.at("market-b"),
        500LL,
        "market-b exposure"
    );
}

void ReservationBook_RejectsUnapprovedDecision() {
    ReservationBook book;
    const auto result = book.try_reserve(
        make_intent(),
        {},
        1'500
    );

    expect_false(result.ok, "reserve ok");
    expect_equal(
        result.reject_reason,
        RiskRejectReason::NotEvaluated,
        "reject reason"
    );
    expect_equal(book.snapshot().active_reservations, 0ULL, "active");
}

void ReservationBook_RejectsDuplicateIdempotencyKey() {
    ReservationBook book;
    const auto decision = make_approved_decision(1, 123);
    const auto first = book.try_reserve(make_intent("same-key"), decision, 1'500);
    const auto second =
        book.try_reserve(make_intent("same-key"), decision, 1'500);

    expect_true(first.ok, "first");
    expect_false(second.ok, "second");
    expect_equal(
        second.reject_reason,
        RiskRejectReason::DuplicateReservation,
        "duplicate reason"
    );
    expect_equal(book.snapshot().active_reservations, 1ULL, "active");
}

void ReservationBook_ExpireOldReleasesLedger() {
    ReservationBook book;
    const auto decision = make_approved_decision(1, 123);
    const auto first = book.try_reserve(make_intent("expiring"), decision, 1'500);
    expect_true(first.ok, "first");

    book.expire_old(2'000);
    auto snapshot = book.snapshot();
    expect_equal(snapshot.active_reservations, 0ULL, "active after expire");
    expect_equal(snapshot.expired_reservations, 1ULL, "expired");
    expect_equal(snapshot.total_reserved_cost_tick, 0LL, "cost released");
    expect_true(snapshot.reserved_asset_lots.empty(), "inventory released");

    auto refreshed = make_intent("expiring");
    refreshed.intent_id = 101;
    refreshed.expires_at_ns = 3'000;
    const auto second = book.try_reserve(refreshed, decision, 2'001);
    expect_true(second.ok, "idempotency key freed after expiry");
    snapshot = book.snapshot();
    expect_equal(snapshot.active_reservations, 1ULL, "active after refresh");
    expect_equal(snapshot.expired_reservations, 1ULL, "expired remains");
}

void ReservationBook_ReleaseFreesIdempotencyKey() {
    ReservationBook book;
    const auto decision = make_approved_decision(1, 123);
    const auto first = book.try_reserve(make_intent("release-key"), decision, 1'500);
    expect_true(first.ok, "first");

    book.release(first.reservation_id);
    const auto second =
        book.try_reserve(make_intent("release-key"), decision, 1'600);

    expect_true(second.ok, "second after release");
    const auto snapshot = book.snapshot();
    expect_equal(snapshot.active_reservations, 1ULL, "active");
    expect_equal(snapshot.released_reservations, 1ULL, "released");
}

void ReservationBook_ReservationIsNotOrder() {
    ReservationBook book;
    const auto decision = make_approved_decision(1, 123);
    const auto result = book.try_reserve(make_intent(), decision, 1'500);

    expect_true(result.ok, "reserve ok");
    expect_true(result.reservation_id != 0, "reservation id");
    expect_true(
        result.reservation_id != make_intent().intent_id,
        "reservation id independent from intent id"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "ReservationBook_ApprovedDecisionReserves",
            &ReservationBook_ApprovedDecisionReserves
        },
        {
            "ReservationBook_RejectsUnapprovedDecision",
            &ReservationBook_RejectsUnapprovedDecision
        },
        {
            "ReservationBook_RejectsDuplicateIdempotencyKey",
            &ReservationBook_RejectsDuplicateIdempotencyKey
        },
        {
            "ReservationBook_ExpireOldReleasesLedger",
            &ReservationBook_ExpireOldReleasesLedger
        },
        {
            "ReservationBook_ReleaseFreesIdempotencyKey",
            &ReservationBook_ReleaseFreesIdempotencyKey
        },
        {
            "ReservationBook_ReservationIsNotOrder",
            &ReservationBook_ReservationIsNotOrder
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
