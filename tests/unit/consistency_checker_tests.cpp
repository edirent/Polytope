#include "feed/integrity/ConsistencyChecker.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::feed::BookLevel;
using trading_engine::feed::ConsistencyChecker;
using trading_engine::feed::ConsistencyCode;
using trading_engine::feed::EntityState;
using trading_engine::feed::EntityStatus;
using trading_engine::feed::NormalizedEvent;
using trading_engine::feed::NormalizedEventType;
using trading_engine::feed::NormalizedSide;
using trading_engine::feed::PriceLevelChange;
using trading_engine::feed::StateApplyCode;
using trading_engine::feed::StateApplyResult;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_equal(
    ConsistencyCode actual,
    ConsistencyCode expected,
    const std::string& field
) {
    if (actual != expected) {
        fail("mismatch: " + field);
    }
}

NormalizedEvent good_snapshot_event() {
    NormalizedEvent event;

    event.packet_id = 1;
    event.event_type = NormalizedEventType::Snapshot;
    event.raw_type = "book";
    event.entity_id = "asset-a";
    event.bids = {
        BookLevel{0.50, 100.0}
    };
    event.asks = {
        BookLevel{0.51, 100.0}
    };

    return event;
}

NormalizedEvent good_delta_event() {
    NormalizedEvent event;

    event.packet_id = 2;
    event.event_type = NormalizedEventType::Delta;
    event.raw_type = "price_change";
    event.entity_id = "asset-a";
    event.changes = {
        PriceLevelChange{NormalizedSide::Bid, 0.50, 25.0}
    };

    return event;
}

StateApplyResult apply_result(StateApplyCode code) {
    StateApplyResult result;

    result.code = code;
    result.entity_id = "asset-a";
    result.message = "test apply result";

    return result;
}

EntityState live_entity() {
    EntityState entity;

    entity.entity_id = "asset-a";
    entity.status = EntityStatus::Live;
    entity.initialized = true;
    entity.book.best_bid = 0.50;
    entity.book.best_ask = 0.51;
    entity.book.bids[0.50] = 100.0;
    entity.book.asks[0.51] = 100.0;

    return entity;
}

void GoodSnapshotOk() {
    ConsistencyChecker checker;
    const auto event = good_snapshot_event();
    const auto applied = apply_result(StateApplyCode::Applied);
    const auto entity = live_entity();

    const auto result = checker.check(event, applied, &entity);

    expect_equal(result.code, ConsistencyCode::Ok, "good snapshot");
}

void DeltaBeforeSnapshot() {
    ConsistencyChecker checker;
    const auto event = good_delta_event();
    const auto applied = apply_result(StateApplyCode::DeltaBeforeSnapshot);
    EntityState entity = live_entity();
    entity.status = EntityStatus::Recovering;
    entity.initialized = false;

    const auto result = checker.check(event, applied, &entity);

    expect_equal(
        result.code,
        ConsistencyCode::DeltaBeforeSnapshot,
        "delta before snapshot"
    );
}

void PriceGreaterThanOneInvalidValue() {
    ConsistencyChecker checker;
    auto event = good_delta_event();
    event.changes.front().price = 1.01;

    const auto result =
        checker.check(event, apply_result(StateApplyCode::Applied), nullptr);

    expect_equal(
        result.code,
        ConsistencyCode::InvalidValue,
        "price greater than one"
    );
}

void NegativeSizeInvalidValue() {
    ConsistencyChecker checker;
    auto event = good_delta_event();
    event.changes.front().size = -1.0;

    const auto result =
        checker.check(event, apply_result(StateApplyCode::Applied), nullptr);

    expect_equal(
        result.code,
        ConsistencyCode::InvalidValue,
        "negative size"
    );
}

void CrossedBook() {
    ConsistencyChecker checker;
    const auto event = good_snapshot_event();
    const auto applied = apply_result(StateApplyCode::Applied);
    EntityState entity = live_entity();

    entity.status = EntityStatus::Inconsistent;
    entity.inconsistent = true;
    entity.book.best_bid = 0.60;
    entity.book.best_ask = 0.50;
    entity.book.crossed = true;

    const auto result = checker.check(event, applied, &entity);

    expect_equal(result.code, ConsistencyCode::CrossedBook, "crossed book");
}

void ExternalBboDiverged() {
    ConsistencyChecker checker;
    const auto event = good_snapshot_event();
    const auto applied = apply_result(StateApplyCode::Applied);
    EntityState entity = live_entity();

    entity.book.external_best_bid = 0.49;
    entity.book.external_best_ask = 0.52;
    entity.book.external_bbo_diverged = true;

    const auto result = checker.check(event, applied, &entity);

    expect_equal(
        result.code,
        ConsistencyCode::ExternalBboDiverged,
        "external BBO divergence"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"GoodSnapshotOk", &GoodSnapshotOk},
        {"DeltaBeforeSnapshot", &DeltaBeforeSnapshot},
        {"PriceGreaterThanOneInvalidValue", &PriceGreaterThanOneInvalidValue},
        {"NegativeSizeInvalidValue", &NegativeSizeInvalidValue},
        {"CrossedBook", &CrossedBook},
        {"ExternalBboDiverged", &ExternalBboDiverged}
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
