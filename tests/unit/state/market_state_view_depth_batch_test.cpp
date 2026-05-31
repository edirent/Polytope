#include "decode/public/NormalizedEvent.h"
#include "decode/public/NormalizedEventBatch.h"
#include "state/MarketStateView.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/MarketStateStore.h"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

using trading_engine::decode::BookLevel;
using trading_engine::decode::NormalizedEvent;
using trading_engine::decode::NormalizedEventBatch;
using trading_engine::decode::NormalizedEventType;
using trading_engine::state::MarketDepthView;
using trading_engine::state::MarketStateStore;
using trading_engine::state::MarketStateView;
using trading_engine::state::from_normalized_batch;

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

NormalizedEvent snapshot_event(
    std::string asset_id,
    std::uint64_t packet_id
) {
    NormalizedEvent event;
    event.packet_id = packet_id;
    event.recv_monotonic_ns = packet_id * 1000;
    event.event_type = NormalizedEventType::Snapshot;
    event.entity_id = asset_id;
    event.asset_id = asset_id;
    event.market_id = "market-1";
    event.raw_type = "book";
    event.bids = {BookLevel{0.49, 10.0}};
    event.asks = {BookLevel{0.51, 20.0}};
    event.tick_size = 0.01;
    return event;
}

void apply_snapshot(
    MarketStateStore& store,
    std::string asset_id,
    std::uint64_t packet_id
) {
    NormalizedEventBatch batch;
    expect_true(batch.push_back(snapshot_event(std::move(asset_id), packet_id)), "push");
    const auto events = from_normalized_batch(batch);
    expect_equal(events.size(), 1ULL, "event count");
    const auto result = store.apply(events.front());
    if (!result.ok()) {
        fail("state apply failed: " + result.message);
    }
}

void DepthBatchReadReadsAllAssets() {
    MarketStateStore store;
    apply_snapshot(store, "asset-a", 10);
    apply_snapshot(store, "asset-b", 11);
    MarketStateView view(store);

    const std::string a = "asset-a";
    const std::string b = "asset-b";
    const std::array<const std::string*, 2> ids{&a, &b};
    const std::array<std::uint32_t, 2> indices{4, 9};
    std::array<MarketDepthView, 2> depth{};

    const auto count = view.get_depth_views(
        std::span<const std::string* const>{ids.data(), ids.size()},
        std::span<const std::uint32_t>{indices.data(), indices.size()},
        depth.data(),
        static_cast<std::uint16_t>(depth.size())
    );

    expect_equal(count, static_cast<std::uint16_t>(2), "depth count");
    expect_equal(depth[0].asset_index, 4U, "asset a index");
    expect_equal(depth[1].asset_index, 9U, "asset b index");
    expect_equal(depth[0].ask_count, static_cast<std::uint16_t>(1), "ask count");
    expect_equal(depth[0].asks[0].price_tick, 51LL, "ask price");
}

void DepthBatchReadRejectsMissingAsset() {
    MarketStateStore store;
    apply_snapshot(store, "asset-a", 10);
    MarketStateView view(store);

    const std::array<std::string_view, 2> ids{"asset-a", "missing"};
    const auto result = view.read_depth_batch_by_asset_id(
        std::span<const std::string_view>{ids.data(), ids.size()}
    );

    expect_false(result.ok, "batch ok");
    expect_equal(result.count, static_cast<std::uint16_t>(1), "found count");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"DepthBatchReadReadsAllAssets", &DepthBatchReadReadsAllAssets},
        {"DepthBatchReadRejectsMissingAsset", &DepthBatchReadRejectsMissingAsset},
    };
    return test_map;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <test-name>\n";
        return 2;
    }

    const auto it = tests().find(argv[1]);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << argv[1] << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << argv[1] << " failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
