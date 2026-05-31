#include "state/MarketStateSnapshot.h"
#include "state/view/MarketDepthView.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::state::MarketDepthView;
using trading_engine::state::MarketStateSnapshot;
using trading_engine::state::PriceLevel;
using trading_engine::state::market_depth_view_from_snapshot;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
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

MarketStateSnapshot snapshot() {
    MarketStateSnapshot out;
    out.entity_id = "asset-1";
    out.version = 42;
    out.snapshot_version_hash = 777;
    out.last_book_update_ns = 123456;
    out.usable_for_depth = true;
    out.bid_count = 1;
    out.ask_count = 1;
    out.bids[0] = PriceLevel{.price_tick = 490000, .price = 0.49, .size = 10.0};
    out.asks[0] = PriceLevel{.price_tick = 510000, .price = 0.51, .size = 20.0};
    return out;
}

void StateMarketDepthView_DefaultsAreSafe() {
    const MarketDepthView view;
    expect_equal(view.asset_index, 0U, "asset index");
    expect_equal(view.book_version, 0ULL, "book version");
    expect_equal(view.snapshot_version_hash, 0ULL, "snapshot hash");
    expect_equal(view.bid_count, static_cast<std::uint16_t>(0), "bid count");
    expect_equal(view.ask_count, static_cast<std::uint16_t>(0), "ask count");
}

void DepthViewContainsBidsAsks() {
    const auto view = market_depth_view_from_snapshot(snapshot(), 7);
    expect_equal(view.asset_index, 7U, "asset index");
    expect_equal(view.bid_count, static_cast<std::uint16_t>(1), "bid count");
    expect_equal(view.ask_count, static_cast<std::uint16_t>(1), "ask count");
    expect_equal(view.bids[0].price_tick, 490000LL, "bid price");
    expect_equal(view.asks[0].price_tick, 510000LL, "ask price");
    expect_true(view.usable_for_depth, "usable for depth");
}

void DepthViewPreservesSnapshotHash() {
    const auto view = market_depth_view_from_snapshot(snapshot(), 3);
    expect_equal(view.book_version, 42ULL, "book version");
    expect_equal(view.snapshot_version_hash, 777ULL, "snapshot hash");
    expect_equal(view.last_ws_recv_ns, 123456ULL, "last ws");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"StateMarketDepthView_DefaultsAreSafe",
         &StateMarketDepthView_DefaultsAreSafe},
        {"DepthViewContainsBidsAsks", &DepthViewContainsBidsAsks},
        {"DepthViewPreservesSnapshotHash", &DepthViewPreservesSnapshotHash},
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
