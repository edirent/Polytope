#include "chain_confirm/FillReconciler.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::chain_confirm::ConfirmedDirection;
using trading_engine::chain_confirm::ConfirmedFill;
using trading_engine::chain_confirm::ConfirmedFillStore;
using trading_engine::chain_confirm::FillMappingStatus;
using trading_engine::chain_confirm::FillReconciler;
using trading_engine::chain_confirm::HintDirection;
using trading_engine::chain_confirm::PendingTradeHint;
using trading_engine::chain_confirm::ReconciliationStatus;

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

PendingTradeHint hint(std::uint64_t id, std::uint64_t ts) {
    PendingTradeHint out;
    out.hint_id = id;
    out.market_id = "market-a";
    out.asset_id = "asset-a";
    out.price_tick = 410000;
    out.size_lots = 25;
    out.ws_recv_monotonic_ns = ts;
    out.packet_id = id;
    out.connection_id = 1;
    out.hint_direction = HintDirection::SellAggressorHint;
    return out;
}

ConfirmedFill fill(std::uint64_t ts, bool removed = false) {
    ConfirmedFill out;
    out.fill_id = "0xtx:9";
    out.market_id = "market-a";
    out.asset_id = "asset-a";
    out.price_tick = 410000;
    out.size_lots = 25;
    out.direction = ConfirmedDirection::BuyAggressor;
    out.mapping_status = FillMappingStatus::Mapped;
    out.block_number = 100;
    out.tx_hash = "0xtx";
    out.log_index = 9;
    out.chain_seen_monotonic_ns = ts;
    out.removed = removed;
    return out;
}

void SyntheticReconciliation_WsHintRequiresChainConfirmation() {
    ConfirmedFillStore store;
    FillReconciler reconciler(&store);

    const auto result = reconciler.on_ws_hint(hint(1, 1000));

    expect_equal(
        result.status,
        ReconciliationStatus::UnmatchedHint,
        "ws hint status"
    );
    expect_true(!result.finalized, "ws hint not finalized");
    expect_equal(store.size(), 0ULL, "no chain store insert");
}

void SyntheticReconciliation_ChainFillConfirmsAndOverridesHint() {
    ConfirmedFillStore store;
    FillReconciler reconciler(&store);

    const auto hint_result = reconciler.on_ws_hint(hint(1, 1000));
    (void)hint_result;
    const auto result = reconciler.on_chain_fill(fill(1200));

    expect_equal(
        result.status,
        ReconciliationStatus::ConfirmedOneToOne,
        "status"
    );
    expect_equal(
        result.hint_direction,
        HintDirection::SellAggressorHint,
        "hint direction"
    );
    expect_equal(
        result.confirmed_direction,
        ConfirmedDirection::BuyAggressor,
        "confirmed direction"
    );
    expect_true(result.finalized, "finalized");
}

void SyntheticReconciliation_AmbiguousIsNotForced() {
    ConfirmedFillStore store;
    FillReconciler reconciler(&store);

    const auto first_hint = reconciler.on_ws_hint(hint(1, 1000));
    const auto second_hint = reconciler.on_ws_hint(hint(2, 1001));
    (void)first_hint;
    (void)second_hint;
    const auto result = reconciler.on_chain_fill(fill(1200));

    expect_equal(result.status, ReconciliationStatus::Ambiguous, "status");
    expect_true(!result.finalized, "not finalized");
}

void SyntheticReconciliation_RemovedLogMarksFillRemoved() {
    ConfirmedFillStore store;
    FillReconciler reconciler(&store);

    ConfirmedFill active = fill(1200);
    const auto active_result = reconciler.on_chain_fill(active);
    (void)active_result;
    active.removed = true;

    const auto result = reconciler.on_chain_fill(active);

    expect_equal(
        result.status,
        ReconciliationStatus::RemovedByReorg,
        "status"
    );
    expect_true(store.get("0xtx:9")->removed, "removed in store");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"SyntheticReconciliation_WsHintRequiresChainConfirmation",
         &SyntheticReconciliation_WsHintRequiresChainConfirmation},
        {"SyntheticReconciliation_ChainFillConfirmsAndOverridesHint",
         &SyntheticReconciliation_ChainFillConfirmsAndOverridesHint},
        {"SyntheticReconciliation_AmbiguousIsNotForced",
         &SyntheticReconciliation_AmbiguousIsNotForced},
        {"SyntheticReconciliation_RemovedLogMarksFillRemoved",
         &SyntheticReconciliation_RemovedLogMarksFillRemoved}
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
