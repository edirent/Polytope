#include "state/MarketStateQueryResult.h"
#include "state/snapshot/SnapshotPublisher.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::state::MarketStateSnapshot;
using trading_engine::state::SnapshotPublisher;
using trading_engine::state::StateQueryError;

constexpr const char* kAssetId = "asset-a";
constexpr const char* kMarketId = "market-a";

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

MarketStateSnapshot snapshot(
    std::int64_t bid_tick,
    std::int64_t ask_tick,
    std::uint64_t version = 0
) {
    MarketStateSnapshot out;
    out.entity_id = kAssetId;
    out.market_id = kMarketId;
    out.version = version;
    out.live = true;
    out.has_bid = true;
    out.has_ask = true;
    out.best_bid_tick = bid_tick;
    out.best_ask_tick = ask_tick;
    out.bid_count = 1;
    out.ask_count = 1;
    out.bids[0].price_tick = bid_tick;
    out.bids[0].price = static_cast<double>(bid_tick) / 1'000'000.0;
    out.bids[0].size = 100.0;
    out.asks[0].price_tick = ask_tick;
    out.asks[0].price = static_cast<double>(ask_tick) / 1'000'000.0;
    out.asks[0].size = 200.0;
    out.state_hash = 12345 + static_cast<std::uint64_t>(bid_tick);
    return out;
}

void SnapshotPublisher_ReadMissingReturnsMissingEntity() {
    SnapshotPublisher publisher;

    const auto result = publisher.read(kAssetId);

    expect_false(result.ok, "read ok");
    expect_equal(result.error, StateQueryError::MissingEntity, "error");
    expect_equal(result.entity_id, std::string{kAssetId}, "entity id");
}

void SnapshotPublisher_PublishThenReadReturnsSnapshot() {
    SnapshotPublisher publisher;
    publisher.publish(snapshot(500000, 540000));

    const auto result = publisher.read(kAssetId);

    expect_true(result.ok, "read ok");
    expect_equal(result.error, StateQueryError::None, "error");
    expect_equal(result.value.entity_id, std::string{kAssetId}, "asset");
    expect_equal(result.value.market_id, std::string{kMarketId}, "market");
    expect_equal(result.value.best_bid_tick, 500000LL, "bid");
    expect_equal(result.value.best_ask_tick, 540000LL, "ask");
    expect_equal(result.value.bid_count, 1U, "bid count");
    expect_equal(result.value.ask_count, 1U, "ask count");
    expect_equal(result.version, result.value.version, "version");
}

void SnapshotPublisher_SecondPublishIncrementsVersion() {
    SnapshotPublisher publisher;

    publisher.publish(snapshot(500000, 540000));
    const auto first = publisher.read(kAssetId);
    publisher.publish(snapshot(510000, 550000));
    const auto second = publisher.read(kAssetId);

    expect_true(first.ok, "first ok");
    expect_true(second.ok, "second ok");
    expect_equal(first.version, 1ULL, "first version");
    expect_equal(second.version, 2ULL, "second version");
    expect_equal(second.value.best_bid_tick, 510000LL, "second bid");
}

void SnapshotPublisher_DoesNotExposeMutableReference() {
    SnapshotPublisher publisher;
    publisher.publish(snapshot(500000, 540000));

    auto first = publisher.read(kAssetId);
    expect_true(first.ok, "first ok");
    first.value.best_bid_tick = 1;
    first.value.bids[0].size = 999999.0;

    const auto second = publisher.read(kAssetId);
    expect_true(second.ok, "second ok");
    expect_equal(second.value.best_bid_tick, 500000LL, "stored bid");
    expect_equal(second.value.bids[0].size, 100.0, "stored size");
}

void SnapshotPublisher_DoubleBufferKeepsPreviousSnapshotValidDuringPublish() {
    SnapshotPublisher publisher;
    publisher.publish(snapshot(500000, 540000));

    const auto previous = publisher.read(kAssetId);
    expect_true(previous.ok, "previous ok");

    publisher.publish(snapshot(520000, 560000));
    const auto current = publisher.read(kAssetId);

    expect_true(current.ok, "current ok");
    expect_equal(previous.value.best_bid_tick, 500000LL, "previous bid");
    expect_equal(previous.value.best_ask_tick, 540000LL, "previous ask");
    expect_equal(previous.version, 1ULL, "previous version");

    expect_equal(current.value.best_bid_tick, 520000LL, "current bid");
    expect_equal(current.value.best_ask_tick, 560000LL, "current ask");
    expect_equal(current.version, 2ULL, "current version");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"SnapshotPublisher_ReadMissingReturnsMissingEntity",
         &SnapshotPublisher_ReadMissingReturnsMissingEntity},
        {"SnapshotPublisher_PublishThenReadReturnsSnapshot",
         &SnapshotPublisher_PublishThenReadReturnsSnapshot},
        {"SnapshotPublisher_SecondPublishIncrementsVersion",
         &SnapshotPublisher_SecondPublishIncrementsVersion},
        {"SnapshotPublisher_DoesNotExposeMutableReference",
         &SnapshotPublisher_DoesNotExposeMutableReference},
        {"SnapshotPublisher_DoubleBufferKeepsPreviousSnapshotValidDuringPublish",
         &SnapshotPublisher_DoubleBufferKeepsPreviousSnapshotValidDuringPublish}
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
