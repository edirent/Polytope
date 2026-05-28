#include "chain_confirm/ConfirmedFillStore.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::chain_confirm::ConfirmedDirection;
using trading_engine::chain_confirm::ConfirmedFill;
using trading_engine::chain_confirm::ConfirmedFillStore;
using trading_engine::chain_confirm::ConfirmedFillStoreCode;

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

ConfirmedFill fill(bool removed = false) {
    ConfirmedFill out;
    out.fill_id = "0xtx:7";
    out.order_hash = "0xorder";
    out.market_id = "market-a";
    out.asset_id = "asset-a";
    out.price_tick = 500000;
    out.size_lots = 1000;
    out.direction = ConfirmedDirection::BuyAggressor;
    out.block_number = 10;
    out.tx_hash = "0xtx";
    out.log_index = 7;
    out.removed = removed;
    return out;
}

void ConfirmedFillStore_IdempotentByTxHashLogIndex() {
    ConfirmedFillStore store;

    const auto first = store.upsert(fill());
    const auto second = store.upsert(fill());

    expect_equal(first.code, ConfirmedFillStoreCode::Inserted, "first");
    expect_equal(second.code, ConfirmedFillStoreCode::Duplicate, "second");
    expect_equal(store.size(), 1ULL, "size");
    expect_equal(store.active_count(), 1ULL, "active");
}

void ConfirmedFillStore_MarksRemovedOnReorg() {
    ConfirmedFillStore store;
    const auto inserted = store.upsert(fill());
    (void)inserted;

    const auto removed = store.upsert(fill(true));
    const auto* stored = store.get("0xtx:7");

    expect_equal(
        removed.code,
        ConfirmedFillStoreCode::MarkedRemoved,
        "removed code"
    );
    expect_true(stored != nullptr, "stored");
    expect_true(stored->removed, "stored removed");
    expect_equal(store.active_count(), 0ULL, "active");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"ConfirmedFillStore_IdempotentByTxHashLogIndex",
         &ConfirmedFillStore_IdempotentByTxHashLogIndex},
        {"ConfirmedFillStore_MarksRemovedOnReorg",
         &ConfirmedFillStore_MarksRemovedOnReorg}
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
