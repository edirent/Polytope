#include "state/EntityStateStore.h"
#include "state/chain/ConfirmedTradeState.h"
#include "state/quality/BookQualityState.h"
#include "state/quality/DataQualityGate.h"
#include "state/quality/ReconciliationState.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::state::BookQuality;
using trading_engine::state::ConfirmedTradeState;
using trading_engine::state::DataQualityGate;
using trading_engine::state::DataQualityInput;
using trading_engine::state::EntityState;
using trading_engine::state::EntityStatus;
using trading_engine::state::ReconciliationState;

constexpr std::uint64_t kNowNs = 20'000'000'000ULL;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
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

EntityState healthy_entity() {
    EntityState entity;
    entity.entity_id = "asset-a";
    entity.status = EntityStatus::Live;
    entity.initialized = true;
    entity.snapshot_count = 1;
    entity.last_update_monotonic_ns = kNowNs - 100'000'000ULL;
    entity.book.best_bid = 0.50;
    entity.book.best_ask = 0.54;
    return entity;
}

ConfirmedTradeState healthy_chain() {
    ConfirmedTradeState state;
    state.version = 1;
    state.last_block_number = 100;
    state.last_chain_seen_ns = kNowNs - 100'000'000ULL;
    state.has_last_trade = true;
    state.last_trade_price_tick = 500000;
    state.last_trade_size_lots = 1000;
    return state;
}

DataQualityInput input_for(
    const EntityState& entity,
    const ConfirmedTradeState& chain
) {
    DataQualityInput input;
    input.entity = &entity;
    input.confirmed_trade_state = &chain;
    input.now_ns = kNowNs;
    return input;
}

void Quality_GoodWhenBookAndChainHealthy() {
    const auto entity = healthy_entity();
    const auto chain = healthy_chain();

    const auto state = DataQualityGate{}.evaluate(input_for(entity, chain));

    expect_equal(state.quality, BookQuality::Good, "quality");
    expect_true(state.ws_live, "ws live");
    expect_true(state.chain_live, "chain live");
    expect_true(state.usable_for_depth, "usable depth");
    expect_true(state.usable_for_signal, "usable signal");
}

void Quality_RecoveringWhenDeltaBeforeSnapshot() {
    EntityState entity;
    entity.entity_id = "asset-a";
    entity.status = EntityStatus::Recovering;
    entity.recovering = true;
    entity.last_update_monotonic_ns = kNowNs - 100'000'000ULL;
    const auto chain = healthy_chain();

    const auto state = DataQualityGate{}.evaluate(input_for(entity, chain));

    expect_equal(state.quality, BookQuality::Recovering, "quality");
    expect_false(state.usable_for_depth, "usable depth");
    expect_false(state.usable_for_signal, "usable signal");
}

void Quality_CrossedWhenBestBidGreaterThanAsk() {
    auto entity = healthy_entity();
    entity.book.best_bid = 0.60;
    entity.book.best_ask = 0.50;
    const auto chain = healthy_chain();

    const auto state = DataQualityGate{}.evaluate(input_for(entity, chain));

    expect_equal(state.quality, BookQuality::Crossed, "quality");
    expect_false(state.usable_for_depth, "usable depth");
    expect_false(state.usable_for_signal, "usable signal");
}

void Quality_StaleWhenWsAgeTooHigh() {
    auto entity = healthy_entity();
    entity.last_update_monotonic_ns = kNowNs - 5'000'000'000ULL;
    const auto chain = healthy_chain();

    const auto state = DataQualityGate{}.evaluate(input_for(entity, chain));

    expect_equal(state.quality, BookQuality::Stale, "quality");
    expect_false(state.ws_live, "ws live");
    expect_true(state.chain_live, "chain live");
    expect_false(state.usable_for_depth, "usable depth");
    expect_false(state.usable_for_signal, "usable signal");
}

void Quality_ChainLaggingWhenChainBehind() {
    const auto entity = healthy_entity();
    auto chain = healthy_chain();
    chain.last_chain_seen_ns = kNowNs - 15'000'000'000ULL;

    const auto state = DataQualityGate{}.evaluate(input_for(entity, chain));

    expect_equal(state.quality, BookQuality::ChainLagging, "quality");
    expect_true(state.ws_live, "ws live");
    expect_false(state.chain_live, "chain live");
    expect_true(state.usable_for_depth, "usable depth");
    expect_false(state.usable_for_signal, "usable signal");
}

void Quality_ChainMismatchWhenReconciliationFails() {
    const auto entity = healthy_entity();
    const auto chain = healthy_chain();

    DataQualityInput input = input_for(entity, chain);
    input.reconciliation.record_mismatch(kNowNs);

    const auto state = DataQualityGate{}.evaluate(input);

    expect_equal(state.quality, BookQuality::ChainMismatch, "quality");
    expect_true(state.ws_live, "ws live");
    expect_true(state.chain_live, "chain live");
    expect_equal(
        state.chain_ws_mismatch_count_recent,
        1U,
        "mismatch count"
    );
    expect_true(state.usable_for_depth, "usable depth");
    expect_false(state.usable_for_signal, "usable signal");
}

void ClosedOrResolved_DisablesSignal() {
    auto closed_entity = healthy_entity();
    closed_entity.closed = true;
    closed_entity.status = EntityStatus::Closed;
    const auto chain = healthy_chain();

    const auto closed = DataQualityGate{}.evaluate(
        input_for(closed_entity, chain)
    );
    expect_equal(closed.quality, BookQuality::Closed, "closed quality");
    expect_false(closed.usable_for_depth, "closed depth");
    expect_false(closed.usable_for_signal, "closed signal");

    auto resolved_entity = healthy_entity();
    resolved_entity.book.resolved = true;

    const auto resolved = DataQualityGate{}.evaluate(
        input_for(resolved_entity, chain)
    );
    expect_equal(resolved.quality, BookQuality::Resolved, "resolved quality");
    expect_false(resolved.usable_for_depth, "resolved depth");
    expect_false(resolved.usable_for_signal, "resolved signal");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"Quality_GoodWhenBookAndChainHealthy",
         &Quality_GoodWhenBookAndChainHealthy},
        {"Quality_RecoveringWhenDeltaBeforeSnapshot",
         &Quality_RecoveringWhenDeltaBeforeSnapshot},
        {"Quality_CrossedWhenBestBidGreaterThanAsk",
         &Quality_CrossedWhenBestBidGreaterThanAsk},
        {"Quality_StaleWhenWsAgeTooHigh",
         &Quality_StaleWhenWsAgeTooHigh},
        {"Quality_ChainLaggingWhenChainBehind",
         &Quality_ChainLaggingWhenChainBehind},
        {"Quality_ChainMismatchWhenReconciliationFails",
         &Quality_ChainMismatchWhenReconciliationFails},
        {"ClosedOrResolved_DisablesSignal",
         &ClosedOrResolved_DisablesSignal}
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
