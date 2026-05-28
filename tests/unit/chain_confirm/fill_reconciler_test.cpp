#include "chain_confirm/FillReconciler.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::chain_confirm::ChainConfirmConfig;
using trading_engine::chain_confirm::ConfirmedDirection;
using trading_engine::chain_confirm::ConfirmedFill;
using trading_engine::chain_confirm::ConfirmedFillStore;
using trading_engine::chain_confirm::FillMappingStatus;
using trading_engine::chain_confirm::FillReconciler;
using trading_engine::chain_confirm::HintDirection;
using trading_engine::chain_confirm::PendingTradeHint;
using trading_engine::chain_confirm::PendingTradeHintRing;
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

PendingTradeHint hint(std::uint64_t id, std::uint64_t ts = 1000) {
    PendingTradeHint out;
    out.hint_id = id;
    out.market_id = "market-a";
    out.asset_id = "asset-a";
    out.price_tick = 500000;
    out.size_lots = 1000;
    out.ws_recv_monotonic_ns = ts;
    out.packet_id = id + 10;
    out.connection_id = 1;
    out.hint_direction = HintDirection::SellAggressorHint;
    out.confidence_bps = 100;
    return out;
}

ConfirmedFill fill(
    std::uint64_t ts = 1200,
    ConfirmedDirection direction = ConfirmedDirection::BuyAggressor,
    bool removed = false
) {
    ConfirmedFill out;
    out.fill_id = "0xtx:1";
    out.order_hash = "0xorder";
    out.market_id = "market-a";
    out.asset_id = "asset-a";
    out.price_tick = 500000;
    out.size_lots = 1000;
    out.direction = direction;
    out.mapping_status = FillMappingStatus::Mapped;
    out.block_number = 100;
    out.tx_hash = "0xtx";
    out.log_index = 1;
    out.chain_seen_monotonic_ns = ts;
    out.removed = removed;
    return out;
}

ChainConfirmConfig config() {
    ChainConfirmConfig out;
    out.pending_window_ms = 5;
    out.expire_unmatched_ms = 10;
    out.max_candidate_matches = 8;
    return out;
}

void PendingTradeHintRing_ExpiresOldHints() {
    PendingTradeHintRing ring;
    ring.push(hint(1, 1000));
    ring.push(hint(2, 20'000'000));

    const auto expired = ring.expire(20'000'001, 10'000'000);

    expect_equal(expired.size(), 1ULL, "expired count");
    expect_equal(expired.front().hint_id, 1ULL, "expired id");
    expect_equal(ring.size(), 1ULL, "remaining");
}

void FillReconciler_OneToOneMatch() {
    ConfirmedFillStore store;
    FillReconciler reconciler(&store, config());

    const auto hint_result = reconciler.on_ws_hint(hint(1, 1000));
    (void)hint_result;
    const auto result = reconciler.on_chain_fill(fill(1200));

    expect_equal(
        result.status,
        ReconciliationStatus::ConfirmedOneToOne,
        "status"
    );
    expect_true(result.finalized, "finalized");
    expect_equal(result.hint_ids.size(), 1ULL, "hint ids");
    expect_equal(
        result.confirmed_direction,
        ConfirmedDirection::BuyAggressor,
        "chain direction overwrites hint"
    );
}

void FillReconciler_AmbiguousSamePriceSize() {
    ConfirmedFillStore store;
    FillReconciler reconciler(&store, config());

    const auto first_hint = reconciler.on_ws_hint(hint(1, 1000));
    const auto second_hint = reconciler.on_ws_hint(hint(2, 1100));
    (void)first_hint;
    (void)second_hint;

    const auto result = reconciler.on_chain_fill(fill(1200));

    expect_equal(result.status, ReconciliationStatus::Ambiguous, "status");
    expect_equal(result.candidate_count, 2U, "candidate count");
    expect_equal(result.hint_ids.size(), 2ULL, "hint ids");
}

void FillReconciler_UnmatchedHintExpires() {
    ConfirmedFillStore store;
    FillReconciler reconciler(&store, config());

    const auto hint_result = reconciler.on_ws_hint(hint(1, 1000));
    (void)hint_result;
    const auto expired = reconciler.expire_unmatched(20'000'001);

    expect_equal(expired.size(), 1ULL, "expired count");
    expect_equal(
        expired.front().status,
        ReconciliationStatus::ExpiredHint,
        "status"
    );
}

void FillReconciler_ChainFillOverwritesWsHint() {
    ConfirmedFillStore store;
    FillReconciler reconciler(&store, config());

    PendingTradeHint ws = hint(1, 1000);
    ws.hint_direction = HintDirection::SellAggressorHint;
    const auto hint_result = reconciler.on_ws_hint(ws);
    (void)hint_result;

    const auto result = reconciler.on_chain_fill(
        fill(1200, ConfirmedDirection::BuyAggressor)
    );

    expect_equal(
        result.status,
        ReconciliationStatus::ConfirmedOneToOne,
        "status"
    );
    expect_equal(
        result.hint_direction,
        HintDirection::SellAggressorHint,
        "hint direction retained"
    );
    expect_equal(
        result.confirmed_direction,
        ConfirmedDirection::BuyAggressor,
        "confirmed direction"
    );
}

void FillReconciler_RemovedLogMarksReorg() {
    ConfirmedFillStore store;
    FillReconciler reconciler(&store, config());

    ConfirmedFill removed = fill(1200);
    const auto inserted = store.upsert(removed);
    (void)inserted;
    removed.removed = true;

    const auto result = reconciler.on_chain_fill(removed);

    expect_equal(
        result.status,
        ReconciliationStatus::RemovedByReorg,
        "status"
    );
    expect_true(store.get("0xtx:1")->removed, "stored removed");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"PendingTradeHintRing_ExpiresOldHints",
         &PendingTradeHintRing_ExpiresOldHints},
        {"FillReconciler_OneToOneMatch", &FillReconciler_OneToOneMatch},
        {"FillReconciler_AmbiguousSamePriceSize",
         &FillReconciler_AmbiguousSamePriceSize},
        {"FillReconciler_UnmatchedHintExpires",
         &FillReconciler_UnmatchedHintExpires},
        {"FillReconciler_ChainFillOverwritesWsHint",
         &FillReconciler_ChainFillOverwritesWsHint},
        {"FillReconciler_RemovedLogMarksReorg",
         &FillReconciler_RemovedLogMarksReorg}
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
