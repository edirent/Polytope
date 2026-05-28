#include "decode/core/DecodePipeline.h"
#include "decode/json/JsonDecodeResult.h"
#include "decode/public/NormalizedEventBatch.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/raw_ingest/RawLogReader.h"
#include "state/MarketStateView.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/MarketStateStore.h"

#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using trading_engine::decode::DecodePipeline;
using trading_engine::decode::JsonDecodeKind;
using trading_engine::decode::NormalizedEventBatch;
using trading_engine::decode::NormalizedEventType;
using trading_engine::feed::RawLogReadResult;
using trading_engine::feed::RawLogReader;
using trading_engine::feed::to_decode_input_view;
using trading_engine::state::MarketStateStore;
using trading_engine::state::MarketStateView;
using trading_engine::state::from_normalized_batch;

constexpr std::uint64_t kLegacyBookHash = 12959912045291989833ULL;

struct Summary {
    std::uint64_t packets_read{0};
    std::uint64_t normalized_events{0};
    std::uint64_t snapshot_events{0};
    std::uint64_t delta_events{0};
    std::uint64_t heartbeat_events{0};
    std::uint64_t decode_errors{0};
    std::uint64_t normalization_errors{0};
    std::uint64_t state_errors{0};
    std::string asset_id;
};

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

std::string market39_fixture_path() {
    constexpr const char* candidates[] = {
        "tests/fixtures/polymarket/market_39.raw",
        "../tests/fixtures/polymarket/market_39.raw"
    };

    for (const char* candidate : candidates) {
        std::ifstream in(candidate, std::ios::binary);
        if (in.good()) {
            return candidate;
        }
    }
    return candidates[0];
}

void WsBookToSnapshot_Market39ReconstructsBook() {
    DecodePipeline pipeline;
    RawLogReader reader(market39_fixture_path());
    MarketStateStore store;
    MarketStateView view(store);
    Summary summary;

    while (true) {
        RawLogReadResult raw = reader.next();
        if (raw.eof()) {
            break;
        }
        if (!raw.ok()) {
            fail("raw read failed: " + raw.message);
        }

        ++summary.packets_read;
        NormalizedEventBatch batch;
        const auto decoded = pipeline.decode(
            to_decode_input_view(*raw.packet),
            &batch
        );
        if (!decoded.ok() &&
            decoded.payload_kind != JsonDecodeKind::NonJsonControl) {
            ++summary.decode_errors;
        }
        if (batch.overflowed) {
            ++summary.normalization_errors;
        }

        for (const auto& event : batch.events) {
            ++summary.normalized_events;
            if (event.event_type == NormalizedEventType::Snapshot) {
                ++summary.snapshot_events;
            } else if (event.event_type == NormalizedEventType::Delta) {
                ++summary.delta_events;
            } else if (event.event_type == NormalizedEventType::Heartbeat) {
                ++summary.heartbeat_events;
            }
            if (summary.asset_id.empty() &&
                event.event_type != NormalizedEventType::Heartbeat &&
                !event.entity_id.empty()) {
                summary.asset_id = event.entity_id;
            }
        }

        for (const auto& event : from_normalized_batch(batch)) {
            const auto result = store.apply(event);
            if (!result.ok()) {
                ++summary.state_errors;
            }
        }
    }

    expect_equal(summary.packets_read, 39ULL, "packets_read");
    expect_equal(summary.normalized_events, 39ULL, "normalized_events");
    expect_equal(summary.snapshot_events, 1ULL, "snapshot_events");
    expect_equal(summary.delta_events, 35ULL, "delta_events");
    expect_equal(summary.heartbeat_events, 3ULL, "heartbeat_events");
    expect_equal(summary.decode_errors, 0ULL, "decode_errors");
    expect_equal(summary.normalization_errors, 0ULL, "normalization_errors");
    expect_equal(summary.state_errors, 0ULL, "state_errors");
    expect_equal(store.global_hash(), kLegacyBookHash, "legacy_book_hash");

    const auto snapshot = view.get_snapshot(summary.asset_id);
    expect_true(snapshot.ok, "snapshot ok");
    expect_true(
        snapshot.value.bid_count + snapshot.value.ask_count > 0U,
        "has book"
    );
    expect_true(!snapshot.value.entity_id.empty(), "asset id");
    expect_true(!snapshot.value.market_id.empty(), "market id");
}

}  // namespace

int main() {
    try {
        WsBookToSnapshot_Market39ReconstructsBook();
        std::cout << "WsBookToSnapshot_Market39ReconstructsBook passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "WsBookToSnapshot_Market39ReconstructsBook failed: "
                  << error.what() << '\n';
        return 1;
    }
}
